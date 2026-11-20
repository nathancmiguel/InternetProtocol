/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/acceptor.h"
#include "net/errors.h"

namespace IPNet {
	using asio::ip::tcp;

	bool MakeListenAddress(const FString& Address, EProtocolType Protocol, asio::ip::address& Out) {
		const bool bV6 = Protocol == EProtocolType::V6 || (Protocol == EProtocolType::Any && Address.IsEmpty());
		if (Address.IsEmpty()) {
			Out = bV6 ? asio::ip::address(asio::ip::address_v6::any()) : asio::ip::address(asio::ip::address_v4::any());
			return true;
		}
		if (Address.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)) {
			Out = bV6 ? asio::ip::address(asio::ip::address_v6::loopback()) : asio::ip::address(asio::ip::address_v4::loopback());
			return true;
		}
		FString Literal = Address;
		if (Literal.StartsWith(TEXT("[")) && Literal.EndsWith(TEXT("]"))) {
			Literal = Literal.Mid(1, Literal.Len() - 2);
		}
		asio::error_code Ec;
		Out = asio::ip::make_address(TCHAR_TO_UTF8(*Literal), Ec);
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
		if (Endpoint.address().is_v6()) {
			// Explicit: dual stack for "::" (Any), IPv6 only for a specific IPv6 address.
			Acceptor.set_option(asio::ip::v6_only(!Endpoint.address().is_unspecified()), OutError);
			OutError.clear();
		}
		Acceptor.set_option(asio::socket_base::reuse_address(bReuseAddress), OutError);
		if (!OutError) {
			Acceptor.bind(Endpoint, OutError);
		}
		if (!OutError) {
			// Out-of-range values (the old default was INT_MAX) are clamped to what the OS accepts.
			const int EffectiveBacklog = Backlog > 0 ? FMath::Min(Backlog, static_cast<int>(asio::socket_base::max_listen_connections))
			                                         : static_cast<int>(asio::socket_base::max_listen_connections);
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
		if (Core->IsStopped()) {
			Finish();
			return;
		}
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
			// Transient failures (e.g. too many open files): report the first one of a series, then back off
			// exponentially (100 ms .. 2 s) instead of busy looping and flooding OnError.
			if (RetryDelayMs == 0 && Events.OnError) {
				Events.OnError(Ec);
			}
			RetryDelayMs = RetryDelayMs == 0 ? 100 : FMath::Min(RetryDelayMs * 2, 2000);
			RetryTimer.expires_after(std::chrono::milliseconds(RetryDelayMs));
			RetryTimer.async_wait([Self = shared_from_this()](const asio::error_code& TimerEc) {
				if (!TimerEc) {
					Self->DoAccept();
				}
			});
			return;
		}
		RetryDelayMs = 0;
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
