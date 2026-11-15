/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmpserversession.h"
#include "rtmp/flv.h"
#include "net/errors.h"
#include "net/ioservice.h"
#include "Misc/ScopeLock.h"

namespace IPNet::Rtmp {
	namespace {
		constexpr uint32 ServerWindowAck = 2500000;
		constexpr uint32 ServerChunkSize = 4096;
		constexpr uint32 PublishStreamId = 1;

		FString ToFString(const std::string& Text) {
			return FString(UTF8_TO_TCHAR(Text.c_str()));
		}

		/** "key?token=..." -> "key" (query parameters are not part of the stream identity). */
		std::string StripQuery(const std::string& Name) {
			const size_t Query = Name.find('?');
			return Query == std::string::npos ? Name : Name.substr(0, Query);
		}
	}

	int64 FRtmpServerSession::FTimestampUnwrapper::Unwrap(uint32 Timestamp) {
		if (bStarted && Timestamp < Last && (Last - Timestamp) > 0x80000000u) {
			++Wraps;
		}
		bStarted = true;
		Last = Timestamp;
		return (Wraps << 32) | Timestamp;
	}

	FRtmpServerSession::FRtmpServerSession(std::weak_ptr<FRtmpServerCore> InCore, uint16 InPort)
		: Core(MoveTemp(InCore)), Port(InPort), Handshake(FRtmpHandshake::ERole::Server) {}

	void FRtmpServerSession::OnConnected(const asio::ip::tcp::endpoint& Remote) {
		RemoteEndpoint = FTcpEndpoint(Remote);
	}

	void FRtmpServerSession::Fail(const TCHAR* Reason) {
		if (bFailed) {
			return;
		}
		bFailed = true;
		UE_LOG(LogAsio, Warning, TEXT("InternetProtocol RTMP: closing connection from %s: %s"), *RemoteEndpoint.Address, Reason);
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			Strong->Close();
		}
	}

	void FRtmpServerSession::OnData(const uint8* Data, size_t Size) {
		if (bFailed) {
			return;
		}
		BytesReceived += Size;
		if (!Handshake.IsDone()) {
			std::vector<uint8> Reply, Leftover;
			const FRtmpHandshake::EResult Result = Handshake.Feed(Data, Size, Reply, Leftover);
			if (Result == FRtmpHandshake::EResult::Error) {
				Fail(TEXT("invalid handshake"));
				return;
			}
			if (!Reply.empty()) {
				if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
					Strong->Send(MoveTemp(Reply));
				}
			}
			if (Result != FRtmpHandshake::EResult::Done || Leftover.empty()) {
				return;
			}
			Reader.Append(Leftover.data(), Leftover.size());
		} else {
			Reader.Append(Data, Size);
		}

		FRtmpMessage Message;
		for (;;) {
			const FRtmpChunkReader::EResult Result = Reader.Next(Message);
			if (Result == FRtmpChunkReader::EResult::NeedMore) {
				break;
			}
			if (Result == FRtmpChunkReader::EResult::Error) {
				Fail(*Reader.GetError());
				return;
			}
			HandleMessage(Message);
			if (bFailed) {
				return;
			}
		}

		if (PeerWindow > 0 && BytesReceived - LastAckSent >= PeerWindow) {
			LastAckSent = BytesReceived;
			SendMessage(MakeAcknowledgement(static_cast<uint32>(BytesReceived & 0xFFFFFFFF)));
		}
	}

	void FRtmpServerSession::HandleMessage(FRtmpMessage& Message) {
		switch (Message.Type) {
		case EMessageType::WindowAckSize:
			if (Message.Payload.size() >= 4) {
				PeerWindow = ReadU32BE(Message.Payload.data());
			}
			break;
		case EMessageType::UserControl:
			if (Message.Payload.size() >= 6 && Message.Payload[0] == 0 && Message.Payload[1] == static_cast<uint8>(EUserControl::PingRequest)) {
				SendMessage(MakeUserControl(EUserControl::PingResponse, ReadU32BE(Message.Payload.data() + 2)));
			}
			break;
		case EMessageType::CommandAmf0:
		case EMessageType::CommandAmf3: {
			// AMF3 command messages start with a format byte (0) followed by AMF0 values.
			const size_t Skip = Message.Type == EMessageType::CommandAmf3 && !Message.Payload.empty() ? 1 : 0;
			std::vector<FAmf0Value> Values;
			if (!Amf0DecodeAll(Message.Payload.data() + Skip, Message.Payload.size() - Skip, Values) || Values.size() < 2 || !Values[0].IsString()) {
				Fail(TEXT("malformed command"));
				return;
			}
			HandleCommand(Values, Message.StreamId);
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
			break; // SetChunkSize/Abort applied by the reader; Ack and bandwidth need no action
		}
	}

	void FRtmpServerSession::SendMessage(const FRtmpMessage& Message) {
		std::shared_ptr<FTransport> Strong = Transport.lock();
		if (!Strong) {
			return;
		}
		std::vector<uint8> Bytes;
		Writer.Write(Message, Bytes);
		Strong->Send(MoveTemp(Bytes));
	}

	void FRtmpServerSession::SendCommand(uint32 StreamId, const std::vector<FAmf0Value>& Values) {
		FRtmpMessage Message;
		Message.Csid = Csid::Command;
		Message.Type = EMessageType::CommandAmf0;
		Message.StreamId = StreamId;
		for (const FAmf0Value& Value : Values) {
			Amf0Encode(Value, Message.Payload);
		}
		SendMessage(Message);
	}

	void FRtmpServerSession::SendStatus(uint32 StreamId, const char* Level, const char* Code, const char* Description) {
		FAmf0Value Status = FAmf0Value::MakeObject();
		Status.Add("level", FAmf0Value::MakeString(Level))
		      .Add("code", FAmf0Value::MakeString(Code))
		      .Add("description", FAmf0Value::MakeString(Description));
		SendCommand(StreamId, {FAmf0Value::MakeString("onStatus"), FAmf0Value::MakeNumber(0), FAmf0Value::MakeNull(), Status});
	}

	void FRtmpServerSession::HandleCommand(const std::vector<FAmf0Value>& Values, uint32 StreamId) {
		const std::string& Name = Values[0].String;
		const double Transaction = Values[1].IsNumber() ? Values[1].Number : 0.0;

		if (Name == "connect") {
			if (bConnected) {
				Fail(TEXT("duplicate connect"));
				return;
			}
			if (Values.size() < 3 || !Values[2].IsObjectLike()) {
				Fail(TEXT("connect without command object"));
				return;
			}
			App = Values[2].GetString("app");
			// Some encoders append the stream key or a query to the app ("live?token" / "live/"): normalize.
			App = StripQuery(App);
			while (!App.empty() && App.back() == '/') {
				App.pop_back();
			}
			bConnected = true;

			SendMessage(MakeWindowAckSize(ServerWindowAck));
			SendMessage(MakeSetPeerBandwidth(ServerWindowAck, 2));
			SendMessage(MakeSetChunkSize(ServerChunkSize));
			Writer.SetChunkSize(ServerChunkSize);

			FAmf0Value Properties = FAmf0Value::MakeObject();
			Properties.Add("fmsVer", FAmf0Value::MakeString("FMS/3,0,1,123")).Add("capabilities", FAmf0Value::MakeNumber(31));
			FAmf0Value Result = FAmf0Value::MakeObject();
			Result.Add("level", FAmf0Value::MakeString("status"))
			      .Add("code", FAmf0Value::MakeString("NetConnection.Connect.Success"))
			      .Add("description", FAmf0Value::MakeString("Connection succeeded."))
			      .Add("objectEncoding", FAmf0Value::MakeNumber(Values[2].GetNumber("objectEncoding", 0.0)));
			SendCommand(0, {FAmf0Value::MakeString("_result"), FAmf0Value::MakeNumber(Transaction), Properties, Result});
			return;
		}

		if (!bConnected) {
			Fail(TEXT("command before connect"));
			return;
		}

		if (Name == "createStream") {
			SendCommand(0, {FAmf0Value::MakeString("_result"), FAmf0Value::MakeNumber(Transaction), FAmf0Value::MakeNull(),
			                FAmf0Value::MakeNumber(PublishStreamId)});
		} else if (Name == "releaseStream" || Name == "FCPublish" || Name == "FCUnpublish") {
			if (Transaction > 0.0) {
				SendCommand(0, {FAmf0Value::MakeString("_result"), FAmf0Value::MakeNumber(Transaction), FAmf0Value::MakeNull(), FAmf0Value()});
			}
			if (Name == "FCUnpublish") {
				StopPublishing();
			}
		} else if (Name == "publish") {
			// publish(transaction, null, name, type)
			const std::string StreamArg = Values.size() > 3 && Values[3].IsString() ? Values[3].String : std::string();
			Publish(StreamArg, Transaction, StreamId);
		} else if (Name == "deleteStream" || Name == "closeStream") {
			StopPublishing();
		} else if (Name == "play" || Name == "play2") {
			SendStatus(StreamId, "error", "NetStream.Play.Failed", "This server only accepts publishing.");
		} else if (Name == "_checkbw" || Name == "getStreamLength") {
			SendCommand(0, {FAmf0Value::MakeString("_result"), FAmf0Value::MakeNumber(Transaction), FAmf0Value::MakeNull(), FAmf0Value::MakeNumber(0)});
		}
		// Unknown commands are ignored (e.g. "_error" replies, "onBWDone").
	}

	void FRtmpServerSession::Publish(const std::string& Name, double Transaction, uint32 StreamId) {
		if (bPublishing) {
			SendStatus(StreamId, "error", "NetStream.Publish.BadName", "Already publishing.");
			return;
		}
		StreamName = StripQuery(Name);
		std::shared_ptr<FRtmpServerCore> StrongCore = Core.lock();
		const FString FApp = ToFString(App);
		const FString FName = ToFString(StreamName);
		if (StreamName.empty() || !StrongCore || !StrongCore->IsStreamNameAllowed(FName)) {
			// Never log the stream name: it is usually the secret stream key.
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol RTMP: publish to app '%s' refused (stream key not allowed)."), *FApp);
			SendStatus(StreamId, "error", "NetStream.Publish.Unauthorized", "Stream key not allowed.");
			Fail(TEXT("unauthorized publish"));
			return;
		}

		Info = FRtmpStreamInfo();
		Info.App = FApp;
		Info.StreamName = FName;
		Info.Port = Port;
		Info.Remote = RemoteEndpoint;

		if (!Hub::BeginPublish(Hub::MakeKey(Port, FApp, FName), shared_from_this())) {
			SendStatus(StreamId, "error", "NetStream.Publish.BadName", "Stream already publishing.");
			Fail(TEXT("stream key already in use"));
			return;
		}
		bPublishing = true;
		SendMessage(MakeUserControl(EUserControl::StreamBegin, StreamId));
		SendStatus(StreamId, "status", "NetStream.Publish.Start", "Publishing.");
		UE_LOG(LogAsio, Log, TEXT("InternetProtocol RTMP: publish started on app '%s' from %s."), *FApp, *RemoteEndpoint.Address);
		StrongCore->NotifyPublishStarted(Info);
	}

	void FRtmpServerSession::StopPublishing() {
		if (!bPublishing) {
			return;
		}
		bPublishing = false;
		Hub::EndPublish(Hub::MakeKey(Port, Info.App, Info.StreamName), this);
		for (const FSinkEntry& Entry : Sinks) {
			Entry.Sink->OnStreamEnded();
		}
		Sinks.Reset();
		bHasVideoHeader = false;
		bHasAudioHeader = false;
		if (std::shared_ptr<FRtmpServerCore> StrongCore = Core.lock()) {
			StrongCore->NotifyPublishEnded(Info);
		}
	}

	void FRtmpServerSession::HandleData(const std::vector<FAmf0Value>& Values) {
		// "@setDataFrame", "onMetaData", {...}  or  "onMetaData", {...}
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
	}

	void FRtmpServerSession::HandleMedia(EMessageType Type, uint32 Timestamp, const uint8* Data, size_t Size) {
		if (!bPublishing || Size == 0) {
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
			if (Packet.bSequenceHeader) {
				VideoHeader = Packet;
				bHasVideoHeader = true;
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
			if (Packet.bSequenceHeader) {
				AudioHeader = Packet;
				bHasAudioHeader = true;
			}
		}
		Deliver(Packet);
	}

	void FRtmpServerSession::HandleAggregate(const FRtmpMessage& Message) {
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
				Fail(TEXT("malformed aggregate message"));
				return;
			}
			if (bFirst) {
				FirstTimestamp = Timestamp;
				bFirst = false;
			}
			// Sub-tag timestamps are relative to the aggregate's own timestamp.
			const uint32 Absolute = Message.Timestamp + (Timestamp - FirstTimestamp);
			if (TagType == static_cast<uint8>(EMessageType::Audio) || TagType == static_cast<uint8>(EMessageType::Video)) {
				HandleMedia(static_cast<EMessageType>(TagType), Absolute, Data + Pos, DataSize);
			}
			Pos += DataSize;
			if (Size - Pos < 4) {
				break;
			}
			Pos += 4; // previous tag size
		}
	}

	void FRtmpServerSession::Deliver(const FRtmpMediaPacket& Packet) {
		for (FSinkEntry& Entry : Sinks) {
			if (Packet.Type == ERtmpPacketType::Video && !Packet.bSequenceHeader && Entry.bWaitKeyframe) {
				if (!Packet.bKeyframe) {
					continue;
				}
				Entry.bWaitKeyframe = false;
			}
			Entry.Sink->OnMediaPacket(Packet);
		}
	}

	void FRtmpServerSession::AttachSink(int64 Handle, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink) {
		std::shared_ptr<FTransport> Strong = Transport.lock();
		if (!Strong) {
			return;
		}
		Strong->Post([Self = shared_from_this(), Handle, Sink]() {
			if (!Self->bPublishing) {
				return;
			}
			for (const FSinkEntry& Entry : Self->Sinks) {
				if (Entry.Handle == Handle) {
					return;
				}
			}
			Sink->OnStreamStarted(Self->Info);
			if (Self->bHasVideoHeader) {
				Sink->OnMediaPacket(Self->VideoHeader);
			}
			if (Self->bHasAudioHeader) {
				Sink->OnMediaPacket(Self->AudioHeader);
			}
			Self->Sinks.Add(FSinkEntry{Handle, Sink, true});
		});
	}

	void FRtmpServerSession::DetachSink(int64 Handle) {
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			Strong->Post([Self = shared_from_this(), Handle]() {
				Self->Sinks.RemoveAll([Handle](const FSinkEntry& Entry) { return Entry.Handle == Handle; });
			});
		}
	}

	void FRtmpServerSession::Abort() {
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			Strong->abort();
		}
	}

	void FRtmpServerSession::OnClosed() {
		StopPublishing();
	}

	// ---------------------------------------------------------------------------------------------------------
	// FRtmpServerCore

	std::shared_ptr<FRtmpServerCore> FRtmpServerCore::Create(FRtmpServerCallbacks Callbacks) {
		return std::make_shared<FRtmpServerCore>(MoveTemp(Callbacks));
	}

	FRtmpServerCore::FRtmpServerCore(FRtmpServerCallbacks InCallbacks) : Callbacks(MoveTemp(InCallbacks)) {}

	bool FRtmpServerCore::Open(const asio::ip::tcp::endpoint& Endpoint, bool bReuseAddress, int Backlog, asio::error_code& OutError) {
		std::weak_ptr<FRtmpServerCore> Weak = shared_from_this();
		FAcceptorEvents Events;
		Events.OnAccept = [Weak](asio::ip::tcp::socket&& Socket) {
			if (std::shared_ptr<FRtmpServerCore> Self = Weak.lock()) {
				Self->OnAccept(MoveTemp(Socket));
			}
		};
		Events.OnError = [Weak](const asio::error_code& Ec) {
			if (std::shared_ptr<FRtmpServerCore> Self = Weak.lock()) {
				Self->NotifyError(Ec);
			}
		};
		// Captured by value: the owner usually drops the core right after Close(), before this runs.
		Events.OnClosed = [OnClosed = Callbacks.OnClosed]() {
			if (OnClosed) {
				OnClosed();
			}
		};
		if (Tls && !Tls->IsValid()) {
			OutError = MakeError(EError::TlsSetupFailed);
			return false;
		}
		Acceptor = FTcpAcceptor::Create(MoveTemp(Events));
		if (!Acceptor) {
			OutError = MakeError(EError::NotRunning);
			return false;
		}
		if (!Acceptor->Open(Endpoint, bReuseAddress, Backlog, OutError)) {
			Acceptor->Close();
			Acceptor.reset();
			return false;
		}
		Port = Acceptor->LocalEndpoint().port();
		Hub::RegisterServer(Port, shared_from_this());
		Acceptor->Start();
		return true;
	}

	asio::ip::tcp::endpoint FRtmpServerCore::GetLocalEndpoint() const {
		return Acceptor ? Acceptor->LocalEndpoint() : asio::ip::tcp::endpoint();
	}

	void FRtmpServerCore::Close() {
		std::shared_ptr<FTcpAcceptor> LocalAcceptor;
		TArray<std::weak_ptr<FRtmpServerSession>> LocalSessions;
		{
			FScopeLock ScopeLock(&Lock);
			LocalAcceptor = MoveTemp(Acceptor);
			Acceptor.reset();
			LocalSessions = MoveTemp(Sessions);
			Sessions.Reset();
		}
		if (LocalAcceptor) {
			Hub::UnregisterServer(Port, this);
			LocalAcceptor->Close();
		}
		for (const std::weak_ptr<FRtmpServerSession>& Weak : LocalSessions) {
			if (std::shared_ptr<FRtmpServerSession> Session = Weak.lock()) {
				Session->Abort();
			}
		}
	}

	void FRtmpServerCore::SetAllowedStreamNames(TArray<FString> Names) {
		FScopeLock ScopeLock(&Lock);
		AllowedNames = MoveTemp(Names);
	}

	bool FRtmpServerCore::IsStreamNameAllowed(const FString& Name) const {
		FScopeLock ScopeLock(&Lock);
		if (AllowedNames.Num() == 0) {
			return true;
		}
		for (const FString& Allowed : AllowedNames) {
			if (Allowed.Equals(Name, ESearchCase::CaseSensitive)) {
				return true;
			}
		}
		return false;
	}

	void FRtmpServerCore::NotifyPublishStarted(const FRtmpStreamInfo& Info) {
		if (Callbacks.OnPublishStarted) {
			Callbacks.OnPublishStarted(Info);
		}
	}

	void FRtmpServerCore::NotifyPublishEnded(const FRtmpStreamInfo& Info) {
		if (Callbacks.OnPublishEnded) {
			Callbacks.OnPublishEnded(Info);
		}
	}

	void FRtmpServerCore::NotifyError(const asio::error_code& Error) {
		if (Callbacks.OnError) {
			Callbacks.OnError(Error);
		}
	}

	void FRtmpServerCore::OnAccept(asio::ip::tcp::socket&& Socket) {
		std::shared_ptr<FRtmpServerSession> Session = std::make_shared<FRtmpServerSession>(weak_from_this(), Port);
		FTransportEvents Events;
		Events.OnConnected = [Session](const asio::ip::tcp::endpoint&, const asio::ip::tcp::endpoint& Remote) { Session->OnConnected(Remote); };
		Events.OnData = [Session](const uint8* Data, size_t Size) { Session->OnData(Data, Size); };
		Events.OnClosed = [Session]() { Session->OnClosed(); };
		FTransportOptions Options;
		// A publisher sends media continuously; 30 s of silence (or a stalled handshake) means it is gone.
		Options.IdleTimeout = std::chrono::seconds(30);
		std::shared_ptr<FTransport> Transport = Tls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events), Options)
		                                            : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events), Options);
		if (!Transport) {
			return;
		}
		Session->Transport = Transport;
		{
			FScopeLock ScopeLock(&Lock);
			Sessions.RemoveAll([](const std::weak_ptr<FRtmpServerSession>& Weak) { return Weak.expired(); });
			Sessions.Add(Session);
		}
		Transport->StartAccepted();
	}
}
