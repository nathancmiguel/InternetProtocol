/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmpplay.h"
#include "rtmp/amf0.h"
#include "rtmp/flv.h"
#include "rtmp/rtmpchunk.h"
#include "rtmp/rtmpcommon.h"
#include "rtmp/rtmphandshake.h"
#include "net/errors.h"
#include "net/ioservice.h"
#include "net/tls.h"
#include "net/transport.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include <atomic>
#include <map>

/*
 * RTMP playback client. Same structure as the publishing client (rtmpclient.cpp): all protocol state lives on one
 * strand; each connection attempt gets a new FTransport whose events are posted to the strand tagged with the
 * attempt id, so events of a dead connection are ignored. Public methods only post work.
 */
namespace IPNet::Rtmp {
	namespace {
		constexpr uint32 PlayClientChunkSize = 4096;
		/** Acknowledgement window we announce to the server. */
		constexpr uint32 PlayClientWindowAck = 2500000;
	}

	class FRtmpPlayCore : public std::enable_shared_from_this<FRtmpPlayCore> {
	public:
		FRtmpPlayCore(std::shared_ptr<FIoCore> InIo, FParsedUrl InUrl, const FRtmpPlayOptions& InOptions,
		              TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> InSink, FRtmpPlayCallbacks InCallbacks)
			: Io(MoveTemp(InIo)),
			  Executor(asio::make_strand(Io->Context)),
			  ReconnectTimer(Executor),
			  PingTimer(Executor),
			  Url(MoveTemp(InUrl)),
			  Options(InOptions),
			  Sink(MoveTemp(InSink)),
			  Callbacks(MoveTemp(InCallbacks)) {
			if (Url.bTls) {
				Tls = MakeTlsConfig(Options.Tls, false);
			}
		}

		// ---- any thread ----
		void Start() { Post([](FRtmpPlayCore& Self) { Self.DoStart(); }); }
		void Stop() { Post([](FRtmpPlayCore& Self) { Self.DoStop(); }); }
		ERtmpPlayState GetState() const { return PublicState.load(); }

		FRtmpPlayStats GetStats() const {
			FRtmpPlayStats Stats;
			Stats.BytesReceived = TotalBytes.load();
			Stats.VideoFramesReceived = VideoFrames.load();
			Stats.AudioFramesReceived = AudioFrames.load();
			Stats.RttMs = RttMs.load();
			Stats.ReconnectAttempts = Attempts.load();
			return Stats;
		}

	private:
		template <typename F>
		void Post(F&& Function) {
			asio::post(Executor, [Self = shared_from_this(), Function = Forward<F>(Function)]() mutable { Function(*Self); });
		}

		void SetState(ERtmpPlayState NewState) {
			if (State == NewState) {
				return;
			}
			State = NewState;
			PublicState.store(NewState);
			if (Callbacks.OnStateChanged) {
				Callbacks.OnStateChanged(NewState);
			}
		}

		void ReportError(const FString& Message) {
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol RTMP play: %s"), *Message);
			if (Callbacks.OnError) {
				Callbacks.OnError(Message);
			}
		}

		// ---- strand ----

		void DoStart() {
			if (State != ERtmpPlayState::Idle && State != ERtmpPlayState::Stopped && State != ERtmpPlayState::Failed) {
				return;
			}
			bStopping = false;
			Attempts.store(0);
			Connect();
		}

		void Connect() {
			if (Url.bTls && (!Tls || !Tls->IsValid())) {
				ReportError(TEXT("TLS configuration is invalid"));
				SetState(ERtmpPlayState::Failed);
				return;
			}
			const uint64 Id = ++ConnectionId;
			std::weak_ptr<FRtmpPlayCore> Weak = shared_from_this();
			FTransportEvents Events;
			Events.OnConnected = [Weak, Id](const asio::ip::tcp::endpoint&, const asio::ip::tcp::endpoint&) {
				if (std::shared_ptr<FRtmpPlayCore> Self = Weak.lock()) {
					Self->Post([Id](FRtmpPlayCore& Core) { Core.OnTransportConnected(Id); });
				}
			};
			Events.OnData = [Weak, Id](const uint8* Data, size_t Size) {
				if (std::shared_ptr<FRtmpPlayCore> Self = Weak.lock()) {
					Self->Post([Id, Bytes = std::vector<uint8>(Data, Data + Size)](FRtmpPlayCore& Core) { Core.OnTransportData(Id, Bytes); });
				}
			};
			Events.OnError = [Weak, Id](const asio::error_code& Ec) {
				if (std::shared_ptr<FRtmpPlayCore> Self = Weak.lock()) {
					Self->Post([Id, Ec](FRtmpPlayCore& Core) {
						if (Core.ConnectionId == Id) {
							Core.ReportError(FString::Printf(TEXT("connection error: %hs"), Ec.message().c_str()));
						}
					});
				}
			};
			Events.OnClosed = [Weak, Id]() {
				if (std::shared_ptr<FRtmpPlayCore> Self = Weak.lock()) {
					Self->Post([Id](FRtmpPlayCore& Core) { Core.OnTransportClosed(Id); });
				}
			};
			FTransportOptions TransportOptions;
			// The server sends media (or pings while waiting for a publisher): 30 s of silence means it is gone.
			TransportOptions.IdleTimeout = std::chrono::seconds(30);
			Transport = Url.bTls ? MakeTlsClient(Tls, MoveTemp(Events), TransportOptions) : MakeTcpClient(MoveTemp(Events), TransportOptions);
			if (!Transport) {
				ReportError(TEXT("network service is not running"));
				SetState(ERtmpPlayState::Failed);
				return;
			}
			Handshake = MakeUnique<FRtmpHandshake>(FRtmpHandshake::ERole::Client);
			Reader = MakeUnique<FRtmpChunkReader>();
			Writer = FRtmpChunkWriter();
			Transactions.clear();
			StreamId = 0;
			NextTransaction = 1;
			BytesReceived = 0;
			LastAck = 0;
			PeerWindow = 0;
			bFatal = false;
			VideoClock.Reset();
			AudioClock.Reset();
			if (State != ERtmpPlayState::Reconnecting) {
				SetState(ERtmpPlayState::Connecting);
			}
			Transport->Connect(Url.Host, Url.Port, EProtocolType::V4);
		}

		void OnTransportConnected(uint64 Id) {
			if (Id != ConnectionId || !Transport) {
				return;
			}
			Write(Handshake->Start());
		}

		void OnTransportData(uint64 Id, const std::vector<uint8>& Bytes) {
			if (Id != ConnectionId || !Transport) {
				return;
			}
			TotalBytes.fetch_add(static_cast<int64>(Bytes.size()));
			if (!Handshake->IsDone()) {
				std::vector<uint8> Reply, Leftover;
				const FRtmpHandshake::EResult Result = Handshake->Feed(Bytes.data(), Bytes.size(), Reply, Leftover);
				if (Result == FRtmpHandshake::EResult::Error) {
					ReportError(TEXT("invalid RTMP handshake from server"));
					Transport->Close();
					return;
				}
				if (Result != FRtmpHandshake::EResult::Done) {
					return;
				}
				Write(MoveTemp(Reply));
				SendMessage(MakeSetChunkSize(PlayClientChunkSize));
				Writer.SetChunkSize(PlayClientChunkSize);
				SendMessage(MakeWindowAckSize(PlayClientWindowAck));
				SendConnect();
				Reader->Append(Leftover.data(), Leftover.size());
			} else {
				Reader->Append(Bytes.data(), Bytes.size());
			}
			BytesReceived += Bytes.size();

			FRtmpMessage Message;
			for (;;) {
				const FRtmpChunkReader::EResult Result = Reader->Next(Message);
				if (Result == FRtmpChunkReader::EResult::NeedMore) {
					break;
				}
				if (Result == FRtmpChunkReader::EResult::Error) {
					ReportError(FString::Printf(TEXT("protocol error from server: %s"), *Reader->GetError()));
					Transport->Close();
					return;
				}
				HandleMessage(Message);
				if (!Transport) {
					return;
				}
			}
			if (PeerWindow > 0 && BytesReceived - LastAck >= PeerWindow) {
				LastAck = BytesReceived;
				SendMessage(MakeAcknowledgement(static_cast<uint32>(BytesReceived & 0xFFFFFFFF)));
			}
		}

		void HandleMessage(const FRtmpMessage& Message) {
			switch (Message.Type) {
			case EMessageType::WindowAckSize:
				if (Message.Payload.size() >= 4) {
					PeerWindow = ReadU32BE(Message.Payload.data());
				}
				break;
			case EMessageType::UserControl:
				if (Message.Payload.size() >= 6) {
					const uint16 Event = static_cast<uint16>((Message.Payload[0] << 8) | Message.Payload[1]);
					const uint32 Value = ReadU32BE(Message.Payload.data() + 2);
					if (Event == static_cast<uint16>(EUserControl::PingRequest)) {
						SendMessage(MakeUserControl(EUserControl::PingResponse, Value));
					} else if (Event == static_cast<uint16>(EUserControl::PingResponse)) {
						RttMs.store(static_cast<double>(NowMs() - Value));
					} else if (Event == static_cast<uint16>(EUserControl::StreamEof)) {
						EndStream();
					}
				}
				break;
			case EMessageType::CommandAmf0:
			case EMessageType::CommandAmf3: {
				const size_t Skip = Message.Type == EMessageType::CommandAmf3 && !Message.Payload.empty() ? 1 : 0;
				std::vector<FAmf0Value> Values;
				if (Amf0DecodeAll(Message.Payload.data() + Skip, Message.Payload.size() - Skip, Values) && Values.size() >= 2 && Values[0].IsString()) {
					HandleCommand(Values);
				}
				break;
			}
			case EMessageType::DataAmf0:
			case EMessageType::DataAmf3: {
				const size_t Skip = Message.Type == EMessageType::DataAmf3 && !Message.Payload.empty() ? 1 : 0;
				std::vector<FAmf0Value> Values;
				if (Amf0DecodeAll(Message.Payload.data() + Skip, Message.Payload.size() - Skip, Values)) {
					HandleData(Values);
				}
				break;
			}
			case EMessageType::Audio:
			case EMessageType::Video:
				HandleMedia(Message.Type, Message.Timestamp, Message.Payload.data(), Message.Payload.size());
				break;
			case EMessageType::Aggregate:
				HandleAggregate(Message);
				break;
			default:
				break;
			}
		}

		void HandleCommand(const std::vector<FAmf0Value>& Values) {
			const std::string& Name = Values[0].String;
			const int32 Transaction = static_cast<int32>(Values[1].IsNumber() ? Values[1].Number : 0.0);

			if (Name == "_result" || Name == "_error") {
				const auto Found = Transactions.find(Transaction);
				if (Found == Transactions.end()) {
					return;
				}
				const std::string Request = Found->second;
				Transactions.erase(Found);
				if (Name == "_error") {
					const std::string Code = Values.size() > 3 ? Values[3].GetString("code") : std::string();
					ReportError(FString::Printf(TEXT("server refused '%s' (%s)"), *ToFString(Request), *ToFString(Code)));
					bFatal = Request == "connect";
					Transport->Close();
					return;
				}
				if (Request == "connect") {
					RttMs.store(static_cast<double>(NowMs() - ConnectSentMs));
					SendCommand(0, "createStream", {FAmf0Value::MakeNull()});
				} else if (Request == "createStream") {
					StreamId = Values.size() > 3 && Values[3].IsNumber() ? static_cast<uint32>(Values[3].Number) : 1;
					SendBufferLength();
					// play(name, start = -1000: live only)
					SendCommand(StreamId, "play", {FAmf0Value::MakeNull(), FAmf0Value::MakeString(Url.StreamName), FAmf0Value::MakeNumber(-1000)}, false);
				}
				return;
			}

			if (Name == "onStatus" && Values.size() > 3) {
				const std::string Code = Values[3].GetString("code");
				const std::string Level = Values[3].GetString("level");
				if (Code == "NetStream.Play.Start") {
					OnPlaying();
				} else if (Code == "NetStream.Play.UnpublishNotify" || Code == "NetStream.Play.Stop" || Code == "NetStream.Play.Complete") {
					EndStream();
				} else if (Level == "error") {
					ReportError(FString::Printf(TEXT("server status %s"), *ToFString(Code)));
					// A wrong or unauthorized key will not fix itself: no reconnection. Other refusals (viewer limit,
					// playback temporarily disabled) are retried with backoff.
					bFatal = Code == "NetStream.Play.StreamNotFound" || Code == "NetStream.Play.Unauthorized" || Code == "NetConnection.Connect.Rejected";
					Transport->Close();
				}
			}
		}

		void HandleData(const std::vector<FAmf0Value>& Values) {
			size_t Index = 0;
			if (Index < Values.size() && Values[Index].IsString() && Values[Index].String == "@setDataFrame") {
				++Index;
			}
			if (Index + 1 >= Values.size() || !Values[Index].IsString() || Values[Index].String != "onMetaData" || !Values[Index + 1].IsObjectLike()) {
				return;
			}
			const FAmf0Value& Meta = Values[Index + 1];
			Info.Width = Meta.GetNumber("width");
			Info.Height = Meta.GetNumber("height");
			Info.FrameRate = Meta.GetNumber("framerate", Meta.GetNumber("fps"));
			Info.VideoDataRateKbps = Meta.GetNumber("videodatarate");
			Info.AudioSampleRate = Meta.GetNumber("audiosamplerate");
			Info.AudioChannels = Meta.GetNumber("audiochannels", Meta.GetNumber("stereo") > 0 ? 2.0 : 0.0);
			Info.Encoder = ToFString(Meta.GetString("encoder"));
			BeginStream();
		}

		void HandleMedia(EMessageType Type, uint32 Timestamp, const uint8* Data, size_t Size) {
			if (State != ERtmpPlayState::Playing || Size == 0) {
				return;
			}
			FRtmpMediaPacket Packet;
			if (Type == EMessageType::Video) {
				FFlvVideoTag Tag;
				if (!ParseVideoTag(Data, Size, Tag) || Tag.PacketType == Flv::AvcEndOfSequence) {
					return;
				}
				Packet.Type = ERtmpPacketType::Video;
				Packet.Codec = Tag.CodecId;
				Packet.bKeyframe = Tag.bKeyframe;
				Packet.bSequenceHeader = Tag.CodecId == Flv::CodecH264 && Tag.PacketType == Flv::AvcSequenceHeader;
				Packet.CtsMs = Tag.CompositionTime;
				Packet.DtsMs = VideoClock.Unwrap(Timestamp);
				Packet.Payload = MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>(Tag.Data, static_cast<int32>(Tag.Size)));
				if (!Packet.bSequenceHeader) {
					VideoFrames.fetch_add(1);
				}
			} else {
				FFlvAudioTag Tag;
				if (!ParseAudioTag(Data, Size, Tag)) {
					return;
				}
				Packet.Type = ERtmpPacketType::Audio;
				Packet.Codec = Tag.SoundFormat;
				Packet.bKeyframe = true;
				Packet.bSequenceHeader = Tag.SoundFormat == Flv::SoundFormatAac && Tag.PacketType == Flv::AacSequenceHeader;
				Packet.DtsMs = AudioClock.Unwrap(Timestamp);
				Packet.Payload = MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>(Tag.Data, static_cast<int32>(Tag.Size)));
				if (!Packet.bSequenceHeader) {
					AudioFrames.fetch_add(1);
				}
			}
			// Some servers send media before (or without) onMetaData.
			BeginStream();
			Sink->OnMediaPacket(Packet);
		}

		void HandleAggregate(const FRtmpMessage& Message) {
			// Sequence of FLV tags: type(1) size(3) ts(3) tsExt(1) streamId(3) data(size) prevSize(4).
			const uint8* Data = Message.Payload.data();
			const size_t Size = Message.Payload.size();
			size_t Pos = 0;
			uint32 FirstTimestamp = 0;
			bool bFirst = true;
			while (Size - Pos >= 11) {
				const uint8 TagType = Data[Pos];
				const uint32 DataSize = ReadU24BE(Data + Pos + 1);
				const uint32 Timestamp = ReadU24BE(Data + Pos + 4) | (static_cast<uint32>(Data[Pos + 7]) << 24);
				Pos += 11;
				if (Size - Pos < DataSize) {
					ReportError(TEXT("malformed aggregate message from server"));
					Transport->Close();
					return;
				}
				if (bFirst) {
					FirstTimestamp = Timestamp;
					bFirst = false;
				}
				const uint32 Absolute = Message.Timestamp + (Timestamp - FirstTimestamp);
				if (TagType == static_cast<uint8>(EMessageType::Audio) || TagType == static_cast<uint8>(EMessageType::Video)) {
					HandleMedia(static_cast<EMessageType>(TagType), Absolute, Data + Pos, DataSize);
				}
				Pos += DataSize;
				if (Size - Pos < 4) {
					break;
				}
				Pos += 4;
			}
		}

		void OnPlaying() {
			Attempts.store(0);
			Info = FRtmpStreamInfo();
			Info.App = ToFString(Url.App);
			// Info.StreamName stays empty: the key is not passed around.
			SetState(ERtmpPlayState::Playing);
			SchedulePing();
		}

		void BeginStream() {
			if (bStreamActive || State != ERtmpPlayState::Playing) {
				return;
			}
			bStreamActive = true;
			Sink->OnStreamStarted(Info);
		}

		void EndStream() {
			if (!bStreamActive) {
				return;
			}
			bStreamActive = false;
			VideoClock.Reset();
			AudioClock.Reset();
			Sink->OnStreamEnded();
		}

		void SendConnect() {
			FAmf0Value Connect = FAmf0Value::MakeObject();
			Connect.Add("app", FAmf0Value::MakeString(Url.App))
			       .Add("flashVer", FAmf0Value::MakeString("LNX 9,0,124,2"))
			       .Add("tcUrl", FAmf0Value::MakeString(Url.TcUrl))
			       .Add("fpad", FAmf0Value::MakeBoolean(false))
			       .Add("capabilities", FAmf0Value::MakeNumber(15))
			       .Add("audioCodecs", FAmf0Value::MakeNumber(0x0400))   // AAC
			       .Add("videoCodecs", FAmf0Value::MakeNumber(0x0080))   // H.264
			       .Add("videoFunction", FAmf0Value::MakeNumber(1));
			ConnectSentMs = NowMs();
			SendCommand(0, "connect", {Connect});
		}

		void SendBufferLength() {
			FRtmpMessage Message;
			Message.Csid = Csid::Control;
			Message.Type = EMessageType::UserControl;
			Message.Payload = {0, static_cast<uint8>(EUserControl::SetBufferLength)};
			PutU32BE(Message.Payload, StreamId);
			PutU32BE(Message.Payload, static_cast<uint32>(FMath::Max(0, Options.BufferMs)));
			SendMessage(Message);
		}

		void SendCommand(uint32 InStreamId, const char* Name, const std::vector<FAmf0Value>& Arguments, bool bExpectReply = true) {
			const int32 Transaction = bExpectReply ? NextTransaction++ : 0;
			if (bExpectReply) {
				Transactions[Transaction] = Name;
			}
			FRtmpMessage Message;
			Message.Csid = InStreamId == 0 ? Csid::Command : Csid::Data;
			Message.Type = EMessageType::CommandAmf0;
			Message.StreamId = InStreamId;
			Amf0Encode(FAmf0Value::MakeString(Name), Message.Payload);
			Amf0Encode(FAmf0Value::MakeNumber(Transaction), Message.Payload);
			for (const FAmf0Value& Argument : Arguments) {
				Amf0Encode(Argument, Message.Payload);
			}
			SendMessage(Message);
		}

		void SendMessage(const FRtmpMessage& Message) {
			std::vector<uint8> Bytes;
			Writer.Write(Message, Bytes);
			Write(MoveTemp(Bytes));
		}

		void Write(std::vector<uint8>&& Bytes) {
			if (Transport && !Bytes.empty()) {
				Transport->Send(MoveTemp(Bytes));
			}
		}

		void SchedulePing() {
			PingTimer.expires_after(std::chrono::seconds(5));
			PingTimer.async_wait([Weak = std::weak_ptr<FRtmpPlayCore>(shared_from_this()), Id = ConnectionId](const asio::error_code& Ec) {
				std::shared_ptr<FRtmpPlayCore> Self = Weak.lock();
				if (Ec || !Self || Self->ConnectionId != Id || Self->State != ERtmpPlayState::Playing) {
					return;
				}
				Self->SendMessage(MakeUserControl(EUserControl::PingRequest, Self->NowMs()));
				Self->SchedulePing();
			});
		}

		void OnTransportClosed(uint64 Id) {
			if (Id != ConnectionId) {
				return;
			}
			Transport.reset();
			PingTimer.cancel();
			EndStream();
			if (bStopping) {
				SetState(ERtmpPlayState::Stopped);
				return;
			}
			const int32 Attempt = Attempts.load() + 1;
			if (bFatal || !Options.bReconnect || (Options.MaxReconnectAttempts > 0 && Attempt > Options.MaxReconnectAttempts)) {
				SetState(ERtmpPlayState::Failed);
				return;
			}
			Attempts.store(Attempt);
			SetState(ERtmpPlayState::Reconnecting);
			if (Callbacks.OnReconnecting) {
				Callbacks.OnReconnecting(Attempt);
			}
			const double Base = FMath::Min(30.0, FMath::Pow(2.0, static_cast<double>(FMath::Min(Attempt - 1, 5))));
			const double Delay = Base * FMath::FRandRange(0.8, 1.2);
			ReconnectTimer.expires_after(std::chrono::milliseconds(static_cast<int64>(Delay * 1000.0)));
			ReconnectTimer.async_wait([Weak = std::weak_ptr<FRtmpPlayCore>(shared_from_this())](const asio::error_code& Ec) {
				std::shared_ptr<FRtmpPlayCore> Self = Weak.lock();
				if (!Ec && Self && !Self->bStopping && Self->State == ERtmpPlayState::Reconnecting) {
					Self->Connect();
				}
			});
		}

		void DoStop() {
			bStopping = true;
			ReconnectTimer.cancel();
			PingTimer.cancel();
			if (!Transport) {
				EndStream();
				SetState(ERtmpPlayState::Stopped);
				return;
			}
			if (State == ERtmpPlayState::Playing && StreamId != 0) {
				SendCommand(0, "deleteStream", {FAmf0Value::MakeNull(), FAmf0Value::MakeNumber(StreamId)}, false);
			}
			Transport->Close();
		}

		uint32 NowMs() const {
			return static_cast<uint32>(FPlatformTime::Seconds() * 1000.0);
		}

		std::shared_ptr<FIoCore> Io;
		asio::strand<asio::io_context::executor_type> Executor;
		asio::steady_timer ReconnectTimer;
		asio::steady_timer PingTimer;
		FParsedUrl Url;
		FRtmpPlayOptions Options;
		TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink;
		FRtmpPlayCallbacks Callbacks;
		std::shared_ptr<const FTlsConfig> Tls;

		std::shared_ptr<FTransport> Transport;
		uint64 ConnectionId = 0;
		TUniquePtr<FRtmpHandshake> Handshake;
		TUniquePtr<FRtmpChunkReader> Reader;
		FRtmpChunkWriter Writer;
		std::map<int32, std::string> Transactions;
		int32 NextTransaction = 1;
		uint32 StreamId = 0;
		uint64 BytesReceived = 0;
		uint64 LastAck = 0;
		uint32 PeerWindow = 0;
		uint32 ConnectSentMs = 0;
		FRtmpStreamInfo Info;
		bool bStreamActive = false;
		FTimestampUnwrapper VideoClock;
		FTimestampUnwrapper AudioClock;
		ERtmpPlayState State = ERtmpPlayState::Idle;
		bool bStopping = false;
		bool bFatal = false;

		std::atomic<ERtmpPlayState> PublicState{ERtmpPlayState::Idle};
		std::atomic<int64> TotalBytes{0};
		std::atomic<int32> VideoFrames{0};
		std::atomic<int32> AudioFrames{0};
		std::atomic<double> RttMs{-1.0};
		std::atomic<int32> Attempts{0};
	};

	namespace {
		class FRtmpPlayHandle final : public IRtmpPlaySession {
		public:
			explicit FRtmpPlayHandle(std::shared_ptr<FRtmpPlayCore> InCore) : Core(MoveTemp(InCore)) {}
			virtual ~FRtmpPlayHandle() override { Core->Stop(); }

			virtual void Start() override { Core->Start(); }
			virtual void Stop() override { Core->Stop(); }
			virtual ERtmpPlayState GetState() const override { return Core->GetState(); }
			virtual FRtmpPlayStats GetStats() const override { return Core->GetStats(); }

		private:
			std::shared_ptr<FRtmpPlayCore> Core;
		};
	}
}

using namespace IPNet;
using namespace IPNet::Rtmp;

TSharedPtr<IRtmpPlaySession, ESPMode::ThreadSafe> IRtmpPlaySession::Create(const FString& Url, const FString& StreamKey, const FRtmpPlayOptions& Options,
	TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink, FRtmpPlayCallbacks Callbacks, FString& OutError) {
	FParsedUrl Parsed;
	if (!ParseRtmpUrl(Url, StreamKey, Parsed, OutError)) {
		return nullptr;
	}
	std::shared_ptr<FIoCore> Io = FIoService::Get();
	if (!Io || !Io->IsRunning()) {
		OutError = TEXT("network service is not running");
		return nullptr;
	}
	std::shared_ptr<FRtmpPlayCore> Core = std::make_shared<FRtmpPlayCore>(Io, MoveTemp(Parsed), Options, MoveTemp(Sink), MoveTemp(Callbacks));
	return MakeShared<FRtmpPlayHandle, ESPMode::ThreadSafe>(MoveTemp(Core));
}
