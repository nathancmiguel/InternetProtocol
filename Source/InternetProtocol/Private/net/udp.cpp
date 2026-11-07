/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/udp.h"
#include "net/errors.h"

namespace IPNet {
	using asio::ip::udp;

	std::shared_ptr<FUdpSocket> FUdpSocket::Create(FUdpEvents Events, size_t ReceiveBufferSize) {
		std::shared_ptr<FIoCore> Core = FIoService::Get();
		if (!Core || !Core->IsRunning()) {
			return nullptr;
		}
		std::shared_ptr<FUdpSocket> Socket = std::make_shared<FUdpSocket>(Core, MoveTemp(Events), ReceiveBufferSize);
		Core->Register(Socket);
		return Socket;
	}

	FUdpSocket::FUdpSocket(std::shared_ptr<FIoCore> InCore, FUdpEvents InEvents, size_t ReceiveBufferSize)
		: Core(MoveTemp(InCore)),
		  Executor(asio::make_strand(Core->Context)),
		  Socket(Executor),
		  Resolver(Executor),
		  Events(MoveTemp(InEvents)),
		  ReceiveSize(FMath::Max<size_t>(ReceiveBufferSize, 1)) {}

	FUdpSocket::~FUdpSocket() {
		Core->Unregister(this);
	}

	void FUdpSocket::Connect(const std::string& Host, const std::string& Port, EProtocolType Protocol) {
		asio::post(Executor, [Self = shared_from_this(), Host, Port, Protocol]() {
			if (Self->State.load() != EState::Idle) {
				return;
			}
			Self->State.store(EState::Connecting);
			Self->Resolver.async_resolve(Protocol == EProtocolType::V6 ? udp::v6() : udp::v4(), Host, Port,
				[Self](const asio::error_code& Ec, const udp::resolver::results_type& Results) {
					Self->OnResolved(Ec, Results);
				});
		});
	}

	void FUdpSocket::OnResolved(const asio::error_code& Ec, const udp::resolver::results_type& Results) {
		if (State.load() != EState::Connecting) {
			return;
		}
		if (Ec) {
			Fail(Ec);
			return;
		}
		if (Results.empty()) {
			Fail(asio::error::host_not_found);
			return;
		}
		const udp::endpoint Remote = Results.begin()->endpoint();
		asio::error_code OpenEc;
		Socket.open(Remote.protocol(), OpenEc);
		if (!OpenEc) {
			// UDP "connect" only sets the default peer and filters incoming datagrams; it does not block.
			Socket.connect(Remote, OpenEc);
		}
		if (OpenEc) {
			Fail(OpenEc);
			return;
		}
		bConnected = true;
		State.store(EState::Open);
		asio::error_code Ignored;
		const udp::endpoint Local = Socket.local_endpoint(Ignored);
		if (Events.OnConnected) {
			Events.OnConnected(Local, Remote);
		}
		if (State.load() == EState::Open) {
			StartReceive();
		}
	}

	bool FUdpSocket::Bind(const udp::endpoint& Endpoint, bool bReuseAddress, asio::error_code& OutError) {
		// Not started yet: no other thread can touch the socket.
		Socket.open(Endpoint.protocol(), OutError);
		if (OutError) {
			return false;
		}
		Socket.set_option(asio::socket_base::reuse_address(bReuseAddress), OutError);
		if (!OutError) {
			Socket.bind(Endpoint, OutError);
		}
		if (OutError) {
			asio::error_code Ignored;
			Socket.close(Ignored);
			return false;
		}
		asio::error_code Ignored;
		LocalBound = Socket.local_endpoint(Ignored);
		return true;
	}

	void FUdpSocket::Start() {
		asio::post(Executor, [Self = shared_from_this()]() {
			if (Self->State.load() != EState::Idle || !Self->Socket.is_open()) {
				return;
			}
			Self->State.store(EState::Open);
			Self->StartReceive();
		});
	}

	void FUdpSocket::StartReceive() {
		const size_t Size = ReceiveSize.load();
		if (ReceiveBuffer.size() != Size) {
			ReceiveBuffer.resize(Size);
		}
		Socket.async_receive_from(asio::buffer(ReceiveBuffer), Sender,
			[Self = shared_from_this()](const asio::error_code& Ec, size_t Bytes) { Self->OnReceive(Ec, Bytes); });
	}

	void FUdpSocket::OnReceive(const asio::error_code& Ec, size_t Bytes) {
		if (State.load() != EState::Open) {
			return;
		}
		if (Ec) {
			if (Ec == asio::error::operation_aborted) {
				return;
			}
			// ICMP port unreachable / truncated datagrams must not kill a UDP socket: report and keep receiving.
			if (Events.OnError) {
				Events.OnError(Ec);
			}
			if (State.load() == EState::Open) {
				StartReceive();
			}
			return;
		}
		if (Events.OnData) {
			Events.OnData(ReceiveBuffer.data(), Bytes, Sender);
		}
		if (State.load() == EState::Open) {
			StartReceive();
		}
	}

	bool FUdpSocket::Send(FBytes Data, FSendCallback Callback) {
		return QueueSend(MoveTemp(Data), nullptr, MoveTemp(Callback));
	}

	bool FUdpSocket::SendTo(FBytes Data, const udp::endpoint& To, FSendCallback Callback) {
		return QueueSend(MoveTemp(Data), &To, MoveTemp(Callback));
	}

	bool FUdpSocket::QueueSend(FBytes&& Data, const udp::endpoint* To, FSendCallback&& Callback) {
		if (State.load() == EState::Closed) {
			return false;
		}
		const size_t Size = Data.size();
		if (PendingBytes.fetch_add(Size) + Size > MaxPendingBytes) {
			PendingBytes.fetch_sub(Size);
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol UDP: send queue limit reached, datagram dropped."));
			return false;
		}
		const bool bHasTarget = To != nullptr;
		const udp::endpoint Target = bHasTarget ? *To : udp::endpoint();
		asio::post(Executor, [Self = shared_from_this(), Data = std::make_shared<FBytes>(MoveTemp(Data)), bHasTarget, Target,
			Callback = MoveTemp(Callback)]() mutable {
			auto Complete = [Self, Data, Callback](const asio::error_code& Ec, size_t Bytes) {
				Self->PendingBytes.fetch_sub(Data->size());
				if (Callback) {
					Callback(Ec, Bytes);
				}
			};
			if (Self->State.load() != EState::Open || (!bHasTarget && !Self->bConnected)) {
				Complete(asio::error::not_connected, 0);
				return;
			}
			// Datagram sends are independent; several may be outstanding. The buffer is owned by the handler.
			if (bHasTarget) {
				Self->Socket.async_send_to(asio::buffer(*Data), Target, MoveTemp(Complete));
			} else {
				Self->Socket.async_send(asio::buffer(*Data), MoveTemp(Complete));
			}
		});
		return true;
	}

	void FUdpSocket::Close() {
		asio::post(Executor, [Self = shared_from_this()]() { Self->Finish(); });
	}

	void FUdpSocket::Fail(const asio::error_code& Ec) {
		if (State.load() == EState::Closed) {
			return;
		}
		if (Ec && Events.OnError) {
			Events.OnError(Ec);
		}
		Finish();
	}

	void FUdpSocket::Finish() {
		if (State.exchange(EState::Closed) == EState::Closed) {
			return;
		}
		asio::error_code Ignored;
		Resolver.cancel();
		Socket.close(Ignored);
		Core->Unregister(this);
		FUdpEvents Released = MoveTemp(Events);
		Events = FUdpEvents();
		if (Released.OnClosed) {
			Released.OnClosed();
		}
	}
}
