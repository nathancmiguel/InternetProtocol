/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "net/ioservice.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
 * Stream transport (TCP or TLS over TCP)
 * --------------------------------------
 * A transport owns one socket bound to its own strand. Its public methods are thread-safe: they only
 * post work to the strand. Events are delivered ON THE STRAND (network thread) through FTransportEvents;
 * the owner (a UObject facade or a protocol layer) is responsible for forwarding them to the Game Thread.
 *
 * Guarantees:
 *  - at most one outstanding read and one outstanding write; sends are queued in order with owned buffers;
 *  - on_closed is invoked exactly once, after which every event callback is released (breaks cycles);
 *  - close() is graceful (flushes the queue, TLS close_notify, bounded by a timeout), abort() is immediate;
 *  - every completion callback of a queued send is invoked exactly once (operation_aborted if dropped).
 */
namespace IPNet {
	using FBytes = std::vector<uint8>;
	using FSendCallback = std::function<void(const asio::error_code&, size_t)>;

	struct FTransportEvents {
		/** Connection (and TLS handshake) established. */
		std::function<void(const asio::ip::tcp::endpoint& Local, const asio::ip::tcp::endpoint& Remote)> OnConnected;
		/** Raw bytes received. The pointer is only valid during the call. */
		std::function<void(const uint8* Data, size_t Size)> OnData;
		/** A real error (orderly remote shutdown is not reported as an error). */
		std::function<void(const asio::error_code& Error)> OnError;
		/** Final event, exactly once. */
		std::function<void()> OnClosed;
	};

	class FTransport;

	/**
	 * Receive backpressure shared by a transport and its owner. The owner adds the size of every message it
	 * hands to the Game Thread (strand) and removes it once the Game Thread consumed it. Above Limit the
	 * transport stops reading (TCP flow control slows the peer down); it resumes below Limit / 2.
	 */
	struct FFlowControl {
		explicit FFlowControl(int64 InLimit) : Limit(FMath::Max<int64>(InLimit, 64 * 1024)) {}

		const int64 Limit;
		std::atomic<int64> Pending{0};
		std::atomic<bool> bPaused{false};
		std::weak_ptr<FTransport> Transport;

		void Add(int64 Bytes) { Pending.fetch_add(Bytes, std::memory_order_acq_rel); }
		bool IsOverLimit() const { return Pending.load(std::memory_order_acquire) > Limit; }
		/** Any thread. */
		void Consume(int64 Bytes);
	};

	struct FTransportOptions {
		/** Closes the connection after this much inactivity, reporting TimedOut (0 = disabled). */
		std::chrono::seconds IdleTimeout{0};
		/** Client: DNS + connect + TLS handshake must complete within this time (0 = no limit). */
		std::chrono::milliseconds ConnectTimeout{15000};
		/**
		 * Accepted connections: TLS handshake and, when bEstablishOnOpen is false, the protocol handshake
		 * (HTTP headers, WebSocket upgrade) must complete within this time (0 = no limit).
		 */
		std::chrono::milliseconds HandshakeTimeout{10000};
		/** False: the owner calls MarkEstablished() once its own handshake completed. */
		bool bEstablishOnOpen = true;
		/** Upper bound of bytes waiting in the send queue; send() fails beyond it. */
		size_t MaxQueuedBytes = 64u * 1024u * 1024u;
		/** Optional receive backpressure (see FFlowControl). */
		std::shared_ptr<FFlowControl> Flow;
		bool bNoDelay = true;
		bool bKeepAlive = false;
	};

	struct FTransportStats {
		uint64 BytesSent = 0;
		uint64 BytesReceived = 0;
		uint64 QueuedBytes = 0;
	};

	class FTransport : public FNetObject {
	public:
		/** Client: resolve + connect (+ TLS handshake). */
		virtual void Connect(const std::string& Host, const std::string& Port, EProtocolType Protocol) = 0;
		/** Server: start an accepted connection (+ TLS server handshake). */
		virtual void StartAccepted() = 0;
		/** Queues bytes. Returns false when closing/closed or when the queue limit is exceeded. */
		virtual bool Send(FBytes Data, FSendCallback Callback = nullptr) = 0;
		/** Graceful close. */
		virtual void Close() = 0;
		/** Closes immediately if the connection is not closed within the given time. */
		virtual void CloseAfter(std::chrono::milliseconds Timeout) = 0;
		/** Runs a function on the transport strand. */
		virtual void Post(std::function<void()> Function) = 0;
		/** The protocol handshake completed: stops the handshake timeout. Thread-safe. */
		virtual void MarkEstablished() = 0;
		/** Resumes reading after a backpressure pause. Thread-safe. */
		virtual void ResumeRead() = 0;
		/** Any thread. */
		virtual FTransportStats GetStats() const = 0;
		virtual bool IsTls() const = 0;
	};

	struct FTlsConfig;

	/** Returns nullptr when the I/O service is not running. */
	std::shared_ptr<FTransport> MakeTcpClient(FTransportEvents Events, const FTransportOptions& Options = {});
	std::shared_ptr<FTransport> MakeTlsClient(std::shared_ptr<const FTlsConfig> Tls, FTransportEvents Events,
	                                          const FTransportOptions& Options = {});
	std::shared_ptr<FTransport> MakeTcpAccepted(asio::ip::tcp::socket&& Socket, FTransportEvents Events,
	                                            const FTransportOptions& Options = {});
	std::shared_ptr<FTransport> MakeTlsAccepted(asio::ip::tcp::socket&& Socket, std::shared_ptr<const FTlsConfig> Tls,
	                                            FTransportEvents Events, const FTransportOptions& Options = {});

	/** True for errors that only mean "the peer closed the connection in an orderly way". */
	bool IsOrderlyClose(const asio::error_code& Error);
}
