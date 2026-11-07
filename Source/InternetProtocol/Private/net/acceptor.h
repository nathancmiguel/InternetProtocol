/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "net/ioservice.h"
#include <functional>
#include <memory>

namespace IPNet {

	struct FAcceptorEvents {
		/** A new connection; the socket is bound to its own strand. Called on the acceptor strand. */
		std::function<void(asio::ip::tcp::socket&& Socket)> OnAccept;
		std::function<void(const asio::error_code& Error)> OnError;
		/** Exactly once. */
		std::function<void()> OnClosed;
	};

	/**
	 * Listening socket. Open() is synchronous (bind errors are reported to the caller immediately) and must
	 * be called before Start(); after Start() every method is thread-safe.
	 */
	class FTcpAcceptor final : public FNetObject, public std::enable_shared_from_this<FTcpAcceptor> {
	public:
		/** Returns nullptr when the I/O service is not running. */
		static std::shared_ptr<FTcpAcceptor> Create(FAcceptorEvents Events);

		FTcpAcceptor(std::shared_ptr<FIoCore> InCore, FAcceptorEvents InEvents);
		virtual ~FTcpAcceptor() override;

		bool Open(const asio::ip::tcp::endpoint& Endpoint, bool bReuseAddress, int Backlog, asio::error_code& OutError);
		asio::ip::tcp::endpoint LocalEndpoint() const { return BoundEndpoint; }

		void Start();
		void Close();
		virtual void abort() override { Close(); }

	private:
		void DoAccept();
		void OnAccepted(const asio::error_code& Error, asio::ip::tcp::socket Socket);
		void Finish();

		std::shared_ptr<FIoCore> Core;
		asio::any_io_executor Executor;
		asio::ip::tcp::acceptor Acceptor;
		asio::steady_timer RetryTimer;
		FAcceptorEvents Events;
		asio::ip::tcp::endpoint BoundEndpoint;
		bool bClosed = false;
	};

	/**
	 * Resolves a listen address without blocking: empty = any, "localhost" = loopback, otherwise an IP
	 * literal. Host names would require a blocking DNS lookup on the calling thread and are rejected.
	 */
	bool MakeListenAddress(const FString& Address, EProtocolType Protocol, asio::ip::address& Out);
}
