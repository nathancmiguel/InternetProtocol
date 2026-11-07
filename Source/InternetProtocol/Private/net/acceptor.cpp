/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/acceptor.h"
#include "net/errors.h"

namespace IPNet {
	using asio::ip::tcp;

	bool MakeListenAddress(const FString& Address, EProtocolType Protocol, asio::ip::address& Out) {
		const bool bV6 = Protocol == EProtocolType::V6;
		if (Address.IsEmpty()) {
			Out = bV6 ? asio::ip::address(asio::ip::address_v6::any()) : asio::ip::address(asio::ip::address_v4::any());
			return true;
		}
		if (Address.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)) {
			Out = bV6 ? asio::ip::address(asio::ip::address_v6::loopback()) : asio::ip::address(asio::ip::address_v4::loopback());
			return true;
		}
		asio::error_code Ec;
		Out = asio::ip::make_address(TCHAR_TO_UTF8(*Address), Ec);
		return !Ec;
	}

	std::shared_ptr<FTcpAcceptor> FTcpAcceptor::Create(FAcceptorEvents Events) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core || !Core->IsRunning()) {
			return nullptr;
		}
		std::shared_ptr<FTcpAcceptor> Acceptor = std::make_shared<FTcpAcceptor>(Core, MoveTemp(Events));
		Core->Register(Acceptor);
		return Acceptor;
	}

	FTcpAcceptor::FTcpAcceptor(std::shared_ptr<FIoCore> InCore, FAcceptorEvents InEvents)
		: Core(MoveTemp(InCore)),
		  Executor(asio::make_strand(Core->Context)),
		  Acceptor(Executor),
		  RetryTimer(Executor),
		  Events(MoveTemp(InEvents)) {}

	FTcpAcceptor::~FTcpAcceptor() {
		Core->Unregister(this);
	}

	bool FTcpAcceptor::Open(const tcp::endpoint& Endpoint, bool bReuseAddress, int Backlog, asio::error_code& OutError) {
		// Not started yet: no other thread can touch the acceptor.
		Acceptor.open(Endpoint.protocol(), OutError);
		if (OutError) {
			return false;
		}
		Acceptor.set_option(asio::socket_base::reuse_address(bReuseAddress), OutError);
		if (!OutError) {
			Acceptor.bind(Endpoint, OutError);
		}
		if (!OutError) {
			const int EffectiveBacklog = Backlog > 0 ? Backlog : asio::socket_base::max_listen_connections;
			Acceptor.listen(EffectiveBacklog, OutError);
		}
		if (OutError) {
			asio::error_code Ignored;
			Acceptor.close(Ignored);
			return false;
		}
		asio::error_code Ignored;
		BoundEndpoint = Acceptor.local_endpoint(Ignored);
		return true;
	}

	void FTcpAcceptor::Start() {
		asio::post(Executor, [Self = shared_from_this()]() { Self->DoAccept(); });
	}

	void FTcpAcceptor::Close() {
		asio::post(Executor, [Self = shared_from_this()]() { Self->Finish(); });
	}

	void FTcpAcceptor::DoAccept() {
		if (bClosed || !Acceptor.is_open()) {
			return;
		}
		// Each accepted socket gets its own strand.
		Acceptor.async_accept(asio::make_strand(Core->Context),
			[Self = shared_from_this()](const asio::error_code& Ec, tcp::socket Socket) {
				Self->OnAccepted(Ec, MoveTemp(Socket));
			});
	}

	void FTcpAcceptor::OnAccepted(const asio::error_code& Ec, tcp::socket Socket) {
		if (bClosed) {
			return;
		}
		if (Ec) {
			if (Ec == asio::error::operation_aborted) {
				return;
			}
			if (Events.OnError) {
				Events.OnError(Ec);
			}
			// Transient failures (e.g. too many open files) must not turn into a busy loop.
			RetryTimer.expires_after(std::chrono::milliseconds(100));
			RetryTimer.async_wait([Self = shared_from_this()](const asio::error_code& TimerEc) {
				if (!TimerEc) {
					Self->DoAccept();
				}
			});
			return;
		}
		if (Events.OnAccept) {
			Events.OnAccept(MoveTemp(Socket));
		}
		DoAccept();
	}

	void FTcpAcceptor::Finish() {
		if (bClosed) {
			return;
		}
		bClosed = true;
		asio::error_code Ignored;
		RetryTimer.cancel();
		Acceptor.close(Ignored);
		Core->Unregister(this);
		FAcceptorEvents Released = MoveTemp(Events);
		Events = FAcceptorEvents();
		if (Released.OnClosed) {
			Released.OnClosed();
		}
	}
}
