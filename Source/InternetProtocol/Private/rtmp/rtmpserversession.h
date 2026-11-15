/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "rtmp/rtmpmedia.h"
#include "rtmp/amf0.h"
#include "rtmp/rtmpchunk.h"
#include "rtmp/rtmphandshake.h"
#include "net/acceptor.h"
#include "net/tls.h"
#include "net/transport.h"
#include "HAL/CriticalSection.h"
#include <functional>
#include <memory>

/*
 * RTMP ingest server (native layer, no UObject).
 * FRtmpServerCore owns the listening socket; each accepted connection gets an FRtmpServerSession that runs
 * entirely on its transport strand: handshake -> chunk stream -> commands (connect, createStream,
 * publish, ...) -> FLV demux -> IRtmpMediaSink fan-out through FRtmpIngestHub.
 * Only "publish" is supported (ingest); "play" is refused.
 */
namespace IPNet::Rtmp {

	struct FRtmpServerCallbacks {
		/** All called on network threads; the owner forwards them to the Game Thread if needed. */
		std::function<void(const FRtmpStreamInfo&)> OnPublishStarted;
		std::function<void(const FRtmpStreamInfo&)> OnPublishEnded;
		std::function<void(const asio::error_code&)> OnError;
		std::function<void()> OnClosed;
	};

	class FRtmpServerCore;

	class FRtmpServerSession : public std::enable_shared_from_this<FRtmpServerSession> {
	public:
		FRtmpServerSession(std::weak_ptr<FRtmpServerCore> InCore, uint16 InPort);

		std::weak_ptr<FTransport> Transport;

		// ---- strand (transport events) ----
		void OnConnected(const asio::ip::tcp::endpoint& Remote);
		void OnData(const uint8* Data, size_t Size);
		void OnClosed();

		// ---- any thread (posted to the strand) ----
		void AttachSink(int64 Handle, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink);
		void DetachSink(int64 Handle);
		void Abort();

	private:
		struct FSinkEntry {
			int64 Handle = 0;
			TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink;
			bool bWaitKeyframe = true;
		};

		struct FTimestampUnwrapper {
			uint32 Last = 0;
			int64 Wraps = 0;
			bool bStarted = false;
			int64 Unwrap(uint32 Timestamp);
		};

		void Fail(const TCHAR* Reason);
		void HandleMessage(FRtmpMessage& Message);
		void HandleCommand(const std::vector<FAmf0Value>& Values, uint32 StreamId);
		void HandleData(const std::vector<FAmf0Value>& Values);
		void HandleMedia(EMessageType Type, uint32 Timestamp, const uint8* Data, size_t Size);
		void HandleAggregate(const FRtmpMessage& Message);
		void Publish(const std::string& Name, double Transaction, uint32 StreamId);
		void StopPublishing();
		void Deliver(const FRtmpMediaPacket& Packet);
		void SendMessage(const FRtmpMessage& Message);
		void SendCommand(uint32 StreamId, const std::vector<FAmf0Value>& Values);
		void SendStatus(uint32 StreamId, const char* Level, const char* Code, const char* Description);

		std::weak_ptr<FRtmpServerCore> Core;
		const uint16 Port;
		FRtmpHandshake Handshake;
		FRtmpChunkReader Reader;
		FRtmpChunkWriter Writer;
		FTcpEndpoint RemoteEndpoint;
		std::string App;
		std::string StreamName;
		bool bConnected = false;
		bool bPublishing = false;
		bool bFailed = false;
		FRtmpStreamInfo Info;
		FRtmpMediaPacket VideoHeader;
		FRtmpMediaPacket AudioHeader;
		bool bHasVideoHeader = false;
		bool bHasAudioHeader = false;
		TArray<FSinkEntry> Sinks;
		FTimestampUnwrapper VideoClock;
		FTimestampUnwrapper AudioClock;
		uint64 BytesReceived = 0;
		uint64 LastAckSent = 0;
		uint32 PeerWindow = 0;
	};

	class FRtmpServerCore : public std::enable_shared_from_this<FRtmpServerCore> {
	public:
		static std::shared_ptr<FRtmpServerCore> Create(FRtmpServerCallbacks Callbacks);

		explicit FRtmpServerCore(FRtmpServerCallbacks InCallbacks);

		/** Synchronous bind/listen; starts accepting on success. */
		bool Open(const asio::ip::tcp::endpoint& Endpoint, bool bReuseAddress, int Backlog, asio::error_code& OutError);
		/** Stops listening and aborts every connection. Thread-safe. */
		void Close();

		uint16 GetPort() const { return Port; }
		asio::ip::tcp::endpoint GetLocalEndpoint() const;

		/** rtmps:// ingest: call before Open. Null = plain RTMP. */
		void SetTls(std::shared_ptr<const FTlsConfig> InTls) { Tls = MoveTemp(InTls); }

		void SetAllowedStreamNames(TArray<FString> Names);
		bool IsStreamNameAllowed(const FString& Name) const;

		// Session -> owner notifications (network threads).
		void NotifyPublishStarted(const FRtmpStreamInfo& Info);
		void NotifyPublishEnded(const FRtmpStreamInfo& Info);
		void NotifyError(const asio::error_code& Error);

	private:
		void OnAccept(asio::ip::tcp::socket&& Socket);

		FRtmpServerCallbacks Callbacks;
		std::shared_ptr<FTcpAcceptor> Acceptor;
		std::shared_ptr<const FTlsConfig> Tls;
		uint16 Port = 0;
		mutable FCriticalSection Lock;
		TArray<FString> AllowedNames;
		TArray<std::weak_ptr<FRtmpServerSession>> Sessions;
	};

	/** Hub internals used by the server layer (public API in rtmp/rtmpmedia.h). */
	namespace Hub {
		FString MakeKey(uint16 Port, const FString& App, const FString& StreamName);
		/** Returns false if another connection is already publishing this key. Attaches registered sinks. */
		bool BeginPublish(const FString& Key, const std::shared_ptr<FRtmpServerSession>& Session);
		void EndPublish(const FString& Key, const FRtmpServerSession* Session);
		void RegisterServer(uint16 Port, const std::shared_ptr<FRtmpServerCore>& Server);
		void UnregisterServer(uint16 Port, const FRtmpServerCore* Server);
	}
}
