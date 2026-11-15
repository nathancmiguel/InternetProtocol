/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmppublish.h"
#include "rtmp/amf0.h"
#include "rtmp/flv.h"
#include "rtmp/rtmpchunk.h"
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
 * RTMP publishing client. All protocol state lives on one strand (Executor); each connection attempt gets a
 * new FTransport whose events are posted to that strand, tagged with the attempt id so events of a dead
 * connection are ignored. Public methods only post work.
 */
namespace IPNet::Rtmp {
	namespace {
		constexpr uint32 ClientChunkSize = 4096;

		struct FParsedUrl {
			bool bTls = false;
			std::string Host;
			std::string Port;
			std::string App;
			std::string StreamName;
			std::string TcUrl;
		};

		bool ParseUrl(const FString& Url, const FString& StreamKey, FParsedUrl& Out, FString& OutError) {
			FString Rest;
			if (Url.StartsWith(TEXT("rtmps://"), ESearchCase::IgnoreCase)) {
				Out.bTls = true;
				Rest = Url.Mid(8);
			} else if (Url.StartsWith(TEXT("rtmp://"), ESearchCase::IgnoreCase)) {
				Rest = Url.Mid(7);
			} else {
				OutError = TEXT("URL must start with rtmp:// or rtmps://");
				return false;
			}
			FString HostPort, Path;
			if (!Rest.Split(TEXT("/"), &HostPort, &Path)) {
				OutError = TEXT("URL has no application path (rtmp://host/app)");
				return false;
			}
			FString Host = HostPort, Port = Out.bTls ? TEXT("443") : TEXT("1935");
			if (HostPort.StartsWith(TEXT("["))) {
				int32 Close;
				if (!HostPort.FindChar(TEXT(']'), Close)) {
					OutError = TEXT("invalid IPv6 host");
					return false;
				}
				Host = HostPort.Mid(1, Close - 1);
				if (HostPort.Len() > Close + 2 && HostPort[Close + 1] == TEXT(':')) {
					Port = HostPort.Mid(Close + 2);
				}
			} else {
				FString Left, Right;
				if (HostPort.Split(TEXT(":"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
					Host = Left;
					Port = Right;
				}
			}
			if (Host.IsEmpty() || !Port.IsNumeric() || FCString::Atoi(*Port) <= 0 || FCString::Atoi(*Port) > 65535) {
				OutError = TEXT("invalid host or port");
				return false;
			}
			while (Path.EndsWith(TEXT("/"))) {
				Path.LeftChopInline(1);
			}
			FString App = Path, Stream = StreamKey;
			if (Stream.IsEmpty()) {
				FString Left, Right;
				if (!Path.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
					OutError = TEXT("no stream key: pass it separately or as rtmp://host/app/KEY");
					return false;
				}
				App = Left;
				Stream = Right;
			}
			if (App.IsEmpty() || Stream.IsEmpty()) {
				OutError = TEXT("empty application or stream key");
				return false;
			}
			Out.Host = TCHAR_TO_UTF8(*Host);
			Out.Port = TCHAR_TO_UTF8(*Port);
			Out.App = TCHAR_TO_UTF8(*App);
			Out.StreamName = TCHAR_TO_UTF8(*Stream);
			const FString TcUrl = FString::Printf(TEXT("%s://%s:%s/%s"), Out.bTls ? TEXT("rtmps") : TEXT("rtmp"),
				Host.Contains(TEXT(":")) ? *FString::Printf(TEXT("[%s]"), *Host) : *Host, *Port, *App);
			Out.TcUrl = TCHAR_TO_UTF8(*TcUrl);
			return true;
		}

		FString ToFString(const std::string& Text) {
			return FString(UTF8_TO_TCHAR(Text.c_str()));
		}
	}

	class FRtmpClientCore : public std::enable_shared_from_this<FRtmpClientCore> {
	public:
		FRtmpClientCore(std::shared_ptr<FIoCore> InIo, FParsedUrl InUrl, const FRtmpPublishOptions& InOptions, FRtmpPublishCallbacks InCallbacks)
			: Io(MoveTemp(InIo)),
			  Executor(asio::make_strand(Io->Context)),
			  ReconnectTimer(Executor),
			  PingTimer(Executor),
			  Url(MoveTemp(InUrl)),
			  Options(InOptions),
			  Callbacks(MoveTemp(InCallbacks)) {
			if (Url.bTls) {
				Tls = MakeTlsConfig(Options.Tls, false);
			}
		}

		// ---- any thread ----
		void Start() { Post([](FRtmpClientCore& Self) { Self.DoStart(); }); }
		void Stop() { Post([](FRtmpClientCore& Self) { Self.DoStop(); }); }

		void SetVideoConfig(TArray<uint8> Config) {
			Post([Config = MoveTemp(Config)](FRtmpClientCore& Self) mutable {
				Self.VideoConfig = MoveTemp(Config);
				if (Self.State == ERtmpPublishState::Publishing) {
					Self.SendVideoConfig();
				}
			});
		}

		void SetAudioConfig(TArray<uint8> Config) {
			Post([Config = MoveTemp(Config)](FRtmpClientCore& Self) mutable {
				Self.AudioConfig = MoveTemp(Config);
				if (Self.State == ERtmpPublishState::Publishing) {
					Self.SendAudioConfig();
				}
			});
		}

		bool SendVideo(int64 DtsMs, int32 CtsMs, bool bKeyframe, TArray<uint8> Data) {
			if (PublicState.load() != ERtmpPublishState::Publishing) {
				++DroppedVideo;
				return false;
			}
			Post([DtsMs, CtsMs, bKeyframe, Data = MoveTemp(Data)](FRtmpClientCore& Self) { Self.DoSendVideo(DtsMs, CtsMs, bKeyframe, Data); });
			return true;
		}

		bool SendAudio(int64 DtsMs, TArray<uint8> Data) {
			if (PublicState.load() != ERtmpPublishState::Publishing) {
				++DroppedAudio;
				return false;
			}
			Post([DtsMs, Data = MoveTemp(Data)](FRtmpClientCore& Self) { Self.DoSendAudio(DtsMs, Data); });
			return true;
		}

		ERtmpPublishState GetState() const { return PublicState.load(); }

		FRtmpPublishStats GetStats() const {
			FRtmpPublishStats Stats;
			Stats.BytesSent = BytesSent.load();
			Stats.QueuedBytes = InFlight.load();
			Stats.VideoFramesSent = VideoSent.load();
			Stats.AudioFramesSent = AudioSent.load();
			Stats.DroppedVideoFrames = DroppedVideo.load();
			Stats.DroppedAudioFrames = DroppedAudio.load();
			Stats.RttMs = RttMs.load();
			Stats.ReconnectAttempts = Attempts.load();
			return Stats;
		}

	private:
		template <typename F>
		void Post(F&& Function) {
			asio::post(Executor, [Self = shared_from_this(), Function = Forward<F>(Function)]() mutable { Function(*Self); });
		}

		void SetState(ERtmpPublishState NewState) {
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
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol RTMP publish: %s"), *Message);
			if (Callbacks.OnError) {
				Callbacks.OnError(Message);
			}
		}

		// ---- strand ----

		void DoStart() {
			if (State != ERtmpPublishState::Idle && State != ERtmpPublishState::Stopped && State != ERtmpPublishState::Failed) {
				return;
			}
			bStopping = false;
			Attempts.store(0);
			Connect();
		}

		void Connect() {
			if (Url.bTls && (!Tls || !Tls->IsValid())) {
				ReportError(TEXT("TLS configuration is invalid"));
				SetState(ERtmpPublishState::Failed);
				return;
			}
			const uint64 Id = ++ConnectionId;
			std::weak_ptr<FRtmpClientCore> Weak = shared_from_this();
			FTransportEvents Events;
			Events.OnConnected = [Weak, Id](const asio::ip::tcp::endpoint&, const asio::ip::tcp::endpoint&) {
				if (std::shared_ptr<FRtmpClientCore> Self = Weak.lock()) {
					Self->Post([Id](FRtmpClientCore& Core) { Core.OnTransportConnected(Id); });
				}
			};
			Events.OnData = [Weak, Id](const uint8* Data, size_t Size) {
				if (std::shared_ptr<FRtmpClientCore> Self = Weak.lock()) {
					Self->Post([Id, Bytes = std::vector<uint8>(Data, Data + Size)](FRtmpClientCore& Core) { Core.OnTransportData(Id, Bytes); });
				}
			};
			Events.OnError = [Weak, Id](const asio::error_code& Ec) {
				if (std::shared_ptr<FRtmpClientCore> Self = Weak.lock()) {
					Self->Post([Id, Ec](FRtmpClientCore& Core) {
						if (Core.ConnectionId == Id) {
							Core.ReportError(FString::Printf(TEXT("connection error: %hs"), Ec.message().c_str()));
						}
					});
				}
			};
			Events.OnClosed = [Weak, Id]() {
				if (std::shared_ptr<FRtmpClientCore> Self = Weak.lock()) {
					Self->Post([Id](FRtmpClientCore& Core) { Core.OnTransportClosed(Id); });
				}
			};
			FTransportOptions TransportOptions;
			TransportOptions.IdleTimeout = std::chrono::seconds(30);
			// Our own congestion policy drops frames before the transport limit is reached.
			TransportOptions.MaxQueuedBytes = static_cast<size_t>(FMath::Max<int64>(Options.MaxQueuedBytes, 1 << 20)) * 2;
			Transport = Url.bTls ? MakeTlsClient(Tls, MoveTemp(Events), TransportOptions) : MakeTcpClient(MoveTemp(Events), TransportOptions);
			if (!Transport) {
				ReportError(TEXT("network service is not running"));
				SetState(ERtmpPublishState::Failed);
				return;
			}
			Handshake = MakeUnique<FRtmpHandshake>(FRtmpHandshake::ERole::Client);
			Reader = MakeUnique<FRtmpChunkReader>();
			Writer = FRtmpChunkWriter();
			Transactions.clear();
			StreamId = 0;
			NextTransaction = 1;
			InFlight.store(0);
			bDropUntilKeyframe = true;
			bFatal = false;
			if (State != ERtmpPublishState::Reconnecting) {
				SetState(ERtmpPublishState::Connecting);
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
				SendMessage(MakeSetChunkSize(ClientChunkSize));
				Writer.SetChunkSize(ClientChunkSize);
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
						const uint32 Now = NowMs();
						RttMs.store(static_cast<double>(Now - Value));
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
					SendCommand(0, "releaseStream", {FAmf0Value::MakeNull(), FAmf0Value::MakeString(Url.StreamName)});
					SendCommand(0, "FCPublish", {FAmf0Value::MakeNull(), FAmf0Value::MakeString(Url.StreamName)});
					SendCommand(0, "createStream", {FAmf0Value::MakeNull()});
				} else if (Request == "createStream") {
					StreamId = Values.size() > 3 && Values[3].IsNumber() ? static_cast<uint32>(Values[3].Number) : 1;
					SendCommand(StreamId, "publish", {FAmf0Value::MakeNull(), FAmf0Value::MakeString(Url.StreamName), FAmf0Value::MakeString("live")});
				}
				return;
			}

			if (Name == "onStatus" && Values.size() > 3) {
				const std::string Code = Values[3].GetString("code");
				const std::string Level = Values[3].GetString("level");
				if (Code == "NetStream.Publish.Start") {
					OnPublishing();
				} else if (Level == "error") {
					ReportError(FString::Printf(TEXT("server status %s"), *ToFString(Code)));
					// Bad/unauthorized stream keys will not fix themselves: do not reconnect.
					bFatal = Code == "NetStream.Publish.BadName" || Code == "NetStream.Publish.Unauthorized" || Code == "NetStream.Publish.Denied";
					Transport->Close();
				}
			}
		}

		void OnPublishing() {
			Attempts.store(0);
			bDropUntilKeyframe = true;
			SetState(ERtmpPublishState::Publishing);
			SendMetadata();
			SendVideoConfig();
			SendAudioConfig();
			SchedulePing();
		}

		void SendConnect() {
			FAmf0Value Info = FAmf0Value::MakeObject();
			Info.Add("app", FAmf0Value::MakeString(Url.App))
			    .Add("type", FAmf0Value::MakeString("nonprivate"))
			    .Add("flashVer", FAmf0Value::MakeString("FMLE/3.0 (compatible; InternetProtocol)"))
			    .Add("tcUrl", FAmf0Value::MakeString(Url.TcUrl));
			ConnectSentMs = NowMs();
			SendCommand(0, "connect", {Info});
		}

		void SendCommand(uint32 InStreamId, const char* Name, const std::vector<FAmf0Value>& Arguments) {
			const int32 Transaction = NextTransaction++;
			Transactions[Transaction] = Name;
			FRtmpMessage Message;
			Message.Csid = Csid::Command;
			Message.Type = EMessageType::CommandAmf0;
			Message.StreamId = InStreamId;
			Amf0Encode(FAmf0Value::MakeString(Name), Message.Payload);
			Amf0Encode(FAmf0Value::MakeNumber(Transaction), Message.Payload);
			for (const FAmf0Value& Argument : Arguments) {
				Amf0Encode(Argument, Message.Payload);
			}
			SendMessage(Message);
		}

		void SendMetadata() {
			FAmf0Value Meta = FAmf0Value::MakeEcmaArray();
			auto AddNumber = [&Meta](const char* Key, double Value) {
				if (Value > 0) {
					Meta.Add(Key, FAmf0Value::MakeNumber(Value));
				}
			};
			AddNumber("width", Options.Width);
			AddNumber("height", Options.Height);
			AddNumber("framerate", Options.FrameRate);
			AddNumber("videodatarate", Options.VideoBitrateKbps);
			AddNumber("videocodecid", Flv::CodecH264);
			AddNumber("audiosamplerate", Options.AudioSampleRate);
			AddNumber("audiochannels", Options.AudioChannels);
			AddNumber("audiodatarate", Options.AudioBitrateKbps);
			AddNumber("audiocodecid", Flv::SoundFormatAac);
			Meta.Add("encoder", FAmf0Value::MakeString("InternetProtocol (Unreal Engine)"));
			FRtmpMessage Message;
			Message.Csid = Csid::Data;
			Message.Type = EMessageType::DataAmf0;
			Message.StreamId = StreamId;
			Amf0Encode(FAmf0Value::MakeString("@setDataFrame"), Message.Payload);
			Amf0Encode(FAmf0Value::MakeString("onMetaData"), Message.Payload);
			Amf0Encode(Meta, Message.Payload);
			SendMessage(Message);
		}

		void SendVideoConfig() {
			if (VideoConfig.Num() == 0) {
				return;
			}
			const std::vector<uint8> Tag = BuildVideoTag(true, Flv::AvcSequenceHeader, 0, VideoConfig.GetData(), VideoConfig.Num());
			SendMedia(Csid::Video, EMessageType::Video, LastVideoTs, Tag);
		}

		void SendAudioConfig() {
			if (AudioConfig.Num() == 0) {
				return;
			}
			const std::vector<uint8> Tag = BuildAacAudioTag(Flv::AacSequenceHeader, AudioConfig.GetData(), AudioConfig.Num());
			SendMedia(Csid::Audio, EMessageType::Audio, LastAudioTs, Tag);
		}

		void DoSendVideo(int64 DtsMs, int32 CtsMs, bool bKeyframe, const TArray<uint8>& Data) {
			if (State != ERtmpPublishState::Publishing) {
				++DroppedVideo;
				return;
			}
			const int64 Queued = InFlight.load();
			const int64 Size = Data.Num() + 16;
			// Congestion: keep keyframes as long as they fit, drop P-frames from half-full on, and once a frame
			// was dropped skip everything until the next keyframe (decoders cannot use a broken GOP).
			const bool bOverHalf = Queued > Options.MaxQueuedBytes / 2;
			const bool bFull = Queued + Size > Options.MaxQueuedBytes;
			if (bFull || (!bKeyframe && (bDropUntilKeyframe || bOverHalf))) {
				bDropUntilKeyframe = true;
				++DroppedVideo;
				return;
			}
			bDropUntilKeyframe = false;
			LastVideoTs = static_cast<uint32>(DtsMs);
			const std::vector<uint8> Tag = BuildVideoTag(bKeyframe, Flv::AvcNalu, CtsMs, Data.GetData(), Data.Num());
			SendMedia(Csid::Video, EMessageType::Video, LastVideoTs, Tag);
			++VideoSent;
		}

		void DoSendAudio(int64 DtsMs, const TArray<uint8>& Data) {
			if (State != ERtmpPublishState::Publishing || InFlight.load() + Data.Num() + 16 > Options.MaxQueuedBytes) {
				++DroppedAudio;
				return;
			}
			LastAudioTs = static_cast<uint32>(DtsMs);
			const std::vector<uint8> Tag = BuildAacAudioTag(Flv::AacRaw, Data.GetData(), Data.Num());
			SendMedia(Csid::Audio, EMessageType::Audio, LastAudioTs, Tag);
			++AudioSent;
		}

		void SendMedia(uint32 InCsid, EMessageType Type, uint32 Timestamp, const std::vector<uint8>& Tag) {
			std::vector<uint8> Bytes;
			Writer.Write(InCsid, Type, StreamId, Timestamp, Tag.data(), Tag.size(), Bytes);
			Write(MoveTemp(Bytes));
		}

		void SendMessage(const FRtmpMessage& Message) {
			std::vector<uint8> Bytes;
			Writer.Write(Message, Bytes);
			Write(MoveTemp(Bytes));
		}

		void Write(std::vector<uint8>&& Bytes) {
			if (!Transport || Bytes.empty()) {
				return;
			}
			const int64 Size = static_cast<int64>(Bytes.size());
			InFlight.fetch_add(Size);
			std::weak_ptr<FRtmpClientCore> Weak = shared_from_this();
			const bool bQueued = Transport->Send(MoveTemp(Bytes), [Weak, Size](const asio::error_code& Ec, size_t Written) {
				if (std::shared_ptr<FRtmpClientCore> Self = Weak.lock()) {
					Self->InFlight.fetch_sub(Size);
					Self->BytesSent.fetch_add(static_cast<int64>(Written));
				}
			});
			if (!bQueued) {
				InFlight.fetch_sub(Size);
			}
		}

		void SchedulePing() {
			PingTimer.expires_after(std::chrono::seconds(5));
			PingTimer.async_wait([Weak = std::weak_ptr<FRtmpClientCore>(shared_from_this()), Id = ConnectionId](const asio::error_code& Ec) {
				std::shared_ptr<FRtmpClientCore> Self = Weak.lock();
				if (Ec || !Self || Self->ConnectionId != Id || Self->State != ERtmpPublishState::Publishing) {
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
			if (bStopping) {
				SetState(ERtmpPublishState::Stopped);
				return;
			}
			const int32 Attempt = Attempts.load() + 1;
			if (bFatal || !Options.bReconnect || (Options.MaxReconnectAttempts > 0 && Attempt > Options.MaxReconnectAttempts)) {
				SetState(ERtmpPublishState::Failed);
				return;
			}
			Attempts.store(Attempt);
			SetState(ERtmpPublishState::Reconnecting);
			if (Callbacks.OnReconnecting) {
				Callbacks.OnReconnecting(Attempt);
			}
			const double Base = FMath::Min(30.0, FMath::Pow(2.0, static_cast<double>(FMath::Min(Attempt - 1, 5))));
			const double Delay = Base * FMath::FRandRange(0.8, 1.2);
			ReconnectTimer.expires_after(std::chrono::milliseconds(static_cast<int64>(Delay * 1000.0)));
			ReconnectTimer.async_wait([Weak = std::weak_ptr<FRtmpClientCore>(shared_from_this())](const asio::error_code& Ec) {
				std::shared_ptr<FRtmpClientCore> Self = Weak.lock();
				if (!Ec && Self && !Self->bStopping && Self->State == ERtmpPublishState::Reconnecting) {
					Self->Connect();
				}
			});
		}

		void DoStop() {
			bStopping = true;
			ReconnectTimer.cancel();
			PingTimer.cancel();
			if (!Transport) {
				SetState(ERtmpPublishState::Stopped);
				return;
			}
			if (State == ERtmpPublishState::Publishing) {
				SendCommand(0, "FCUnpublish", {FAmf0Value::MakeNull(), FAmf0Value::MakeString(Url.StreamName)});
				SendCommand(0, "deleteStream", {FAmf0Value::MakeNull(), FAmf0Value::MakeNumber(StreamId)});
			}
			// Graceful: queued media and the unpublish commands are flushed first.
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
		FRtmpPublishOptions Options;
		FRtmpPublishCallbacks Callbacks;
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
		uint32 LastVideoTs = 0;
		uint32 LastAudioTs = 0;
		TArray<uint8> VideoConfig;
		TArray<uint8> AudioConfig;
		ERtmpPublishState State = ERtmpPublishState::Idle;
		bool bStopping = false;
		bool bFatal = false;
		bool bDropUntilKeyframe = true;

		std::atomic<ERtmpPublishState> PublicState{ERtmpPublishState::Idle};
		std::atomic<int64> InFlight{0};
		std::atomic<int64> BytesSent{0};
		std::atomic<int32> VideoSent{0};
		std::atomic<int32> AudioSent{0};
		std::atomic<int32> DroppedVideo{0};
		std::atomic<int32> DroppedAudio{0};
		std::atomic<double> RttMs{-1.0};
		std::atomic<int32> Attempts{0};
	};

	namespace {
		class FRtmpPublishHandle final : public IRtmpPublishSession {
		public:
			explicit FRtmpPublishHandle(std::shared_ptr<FRtmpClientCore> InCore) : Core(MoveTemp(InCore)) {}
			virtual ~FRtmpPublishHandle() override { Core->Stop(); }

			virtual void Start() override { Core->Start(); }
			virtual void Stop() override { Core->Stop(); }
			virtual void SetVideoConfig(TArray<uint8> Config) override { Core->SetVideoConfig(MoveTemp(Config)); }
			virtual void SetAudioConfig(TArray<uint8> Config) override { Core->SetAudioConfig(MoveTemp(Config)); }
			virtual bool SendVideo(int64 DtsMs, int32 CtsMs, bool bKeyframe, TArray<uint8> Avcc) override { return Core->SendVideo(DtsMs, CtsMs, bKeyframe, MoveTemp(Avcc)); }
			virtual bool SendAudio(int64 DtsMs, TArray<uint8> RawAac) override { return Core->SendAudio(DtsMs, MoveTemp(RawAac)); }
			virtual ERtmpPublishState GetState() const override { return Core->GetState(); }
			virtual FRtmpPublishStats GetStats() const override { return Core->GetStats(); }

		private:
			std::shared_ptr<FRtmpClientCore> Core;
		};
	}
}

using namespace IPNet;
using namespace IPNet::Rtmp;

TSharedPtr<IRtmpPublishSession, ESPMode::ThreadSafe> IRtmpPublishSession::Create(const FString& Url, const FString& StreamKey,
	const FRtmpPublishOptions& Options, FRtmpPublishCallbacks Callbacks, FString& OutError) {
	FParsedUrl Parsed;
	if (!ParseUrl(Url, StreamKey, Parsed, OutError)) {
		return nullptr;
	}
	std::shared_ptr<FIoCore> Io = FIoService::Get();
	if (!Io || !Io->IsRunning()) {
		OutError = TEXT("network service is not running");
		return nullptr;
	}
	std::shared_ptr<FRtmpClientCore> Core = std::make_shared<FRtmpClientCore>(Io, MoveTemp(Parsed), Options, MoveTemp(Callbacks));
	return MakeShared<FRtmpPublishHandle, ESPMode::ThreadSafe>(MoveTemp(Core));
}

FString IRtmpPublishSession::MaskUrl(const FString& Url) {
	// scheme://host[:port]/app[/anything...][?query]  ->  scheme://host[:port]/app/***
	const int32 SchemeEnd = Url.Find(TEXT("://"));
	if (SchemeEnd == INDEX_NONE) {
		return TEXT("***");
	}
	const int32 PathStart = Url.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeEnd + 3);
	if (PathStart == INDEX_NONE) {
		return Url;
	}
	FString Path = Url.Mid(PathStart + 1);
	int32 Query;
	if (Path.FindChar(TEXT('?'), Query)) {
		Path.LeftInline(Query);
	}
	int32 Slash;
	const FString App = Path.FindChar(TEXT('/'), Slash) ? Path.Left(Slash) : Path;
	return Url.Left(PathStart + 1) + App + TEXT("/***");
}
