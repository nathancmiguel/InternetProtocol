/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "net/ioservice.h"
#include "net/transport.h"
#include <atomic>
#include <functional>
#include <memory>

namespace IPNet {

	struct FUdpEvents {
		/** Client only: the socket is connected to the resolved peer. */
		std::function<void(const asio::ip::udp::endpoint& Local, const asio::ip::udp::endpoint& Remote)> OnConnected;
		/** One datagram. The pointer is only valid during the call. */
		std::function<void(const uint8* Data, size_t Size, const asio::ip::udp::endpoint& From)> OnData;
		std::function<void(const asio::error_code& Error)> OnError;
		/** Exactly once. */
		std::function<void()> OnClosed;
	};

	/**
	 * UDP socket on its own strand. Client mode: Connect() (async resolve + connect), then Send().
	 * Server mode: Bind() (synchronous, before Start()), then SendTo(). Events are delivered on the strand.
	 */
	class FUdpSocket final : public FNetObject, public std::enable_shared_from_this<FUdpSocket> {
	public:
		/** Returns nullptr when the I/O service is not running. */
		static std::shared_ptr<FUdpSocket> Create(FUdpEvents Events, size_t ReceiveBufferSize);

		FUdpSocket(std::shared_ptr<FIoCore> InCore, FUdpEvents InEvents, size_t ReceiveBufferSize);
		virtual ~FUdpSocket() override;

		void Connect(const std::string& Host, const std::string& Port, EProtocolType Protocol);

		bool Bind(const asio::ip::udp::endpoint& Endpoint, bool bReuseAddress, asio::error_code& OutError);
		asio::ip::udp::endpoint BoundEndpoint() const { return LocalBound; }
		void Start();

		bool Send(FBytes Data, FSendCallback Callback);
		bool SendTo(FBytes Data, const asio::ip::udp::endpoint& To, FSendCallback Callback);

		/** Applies to the next receive operation. */
		void SetReceiveBufferSize(size_t Size) { ReceiveSize.store(FMath::Max<size_t>(Size, 1)); }

		void Close();
		virtual void abort() override { Close(); }

	private:
		enum class EState : uint8 { Idle, Connecting, Open, Closed };

		void OnResolved(const asio::error_code& Ec, const asio::ip::udp::resolver::results_type& Results);
		void StartReceive();
		void OnReceive(const asio::error_code& Ec, size_t Bytes);
		bool QueueSend(FBytes&& Data, const asio::ip::udp::endpoint* To, FSendCallback&& Callback);
		void Fail(const asio::error_code& Ec);
		void Finish();

		std::shared_ptr<FIoCore> Core;
		asio::any_io_executor Executor;
		asio::ip::udp::socket Socket;
		asio::ip::udp::resolver Resolver;
		FUdpEvents Events;
		std::vector<uint8> ReceiveBuffer;
		asio::ip::udp::endpoint Sender;
		asio::ip::udp::endpoint LocalBound;
		std::atomic<size_t> ReceiveSize;
		std::atomic<size_t> PendingBytes{0};
		std::atomic<EState> State{EState::Idle};
		bool bConnected = false;

		static constexpr size_t MaxPendingBytes = 16u * 1024u * 1024u;
	};
}
