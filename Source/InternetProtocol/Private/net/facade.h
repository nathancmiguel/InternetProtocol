/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/fwd.h"
#include "net/acceptor.h"
#include "net/errors.h"
#include "net/gamethread.h"
#include "net/http.h"
#include "net/tls.h"
#include "net/transport.h"
#include "net/udp.h"
#include <atomic>

/*
 * Game Thread helpers shared by the UObject facades. Every function here runs on the Game Thread except
 * the lambdas stored in FTransportEvents / FAcceptorEvents, which run on a network strand and only copy
 * data and post it to the Game Thread with RunOnGameThread (weak UObject + session id).
 */
namespace IPNet {

	inline uint64 NextSessionId() {
		static std::atomic<uint64> Counter{0};
		return ++Counter;
	}

	inline std::string ToStd(const FString& Text) {
		return std::string(TCHAR_TO_UTF8(*Text));
	}

	/** The send-completion callback forwarded to the Game Thread. */
	template <typename TOwner, typename TDelegate>
	FSendCallback MakeSendCallback(TOwner* Owner, const TDelegate& Callback) {
		if (!Callback.IsBound()) {
			return nullptr;
		}
		TWeakObjectPtr<TOwner> Weak(Owner);
		return [Weak, Callback](const asio::error_code& Ec, size_t Bytes) {
			RunOnGameThread(Weak, [Callback, Ec, Bytes](TOwner*) {
				Callback.ExecuteIfBound(FErrorCode(Ec), static_cast<int32>(Bytes));
			});
		};
	}

	struct FFacadeAccess {
		// ------------------------------------------------------------------------------------------------
		// Plain TCP stream facades (UTCPClient, UTCPClientSsl, UTCPRemote, UTCPRemoteSsl).
		// Required members: FIpStreamState Net; OnConnected(); OnMessage(Buffer, Bytes); OnClose(); OnError(Error)
		// Optional hook: void HandleClosedNative() (remotes notify their server).

		template <typename T>
		static FTransportEvents MakeTcpEvents(T* Owner, uint64 Id) {
			TWeakObjectPtr<T> Weak(Owner);
			FTransportEvents Events;
			Events.OnConnected = [Weak, Id](const asio::ip::tcp::endpoint& Local, const asio::ip::tcp::endpoint& Remote) {
				RunOnGameThread(Weak, [Id, Local, Remote](T* Self) {
					if (Self->Net.SessionId != Id) {
						return;
					}
					Self->Net.bOpen = true;
					Self->Net.Local = FTcpEndpoint(Local);
					Self->Net.Remote = FTcpEndpoint(Remote);
					Self->OnConnected.Broadcast();
				});
			};
			Events.OnData = [Weak, Id](const uint8* Data, size_t Size) {
				TArray<uint8> Buffer(Data, static_cast<int32>(Size));
				RunOnGameThread(Weak, [Id, Buffer = MoveTemp(Buffer)](T* Self) {
					if (Self->Net.SessionId == Id) {
						Self->OnMessage.Broadcast(Buffer, Buffer.Num());
					}
				});
			};
			Events.OnError = MakeErrorEvent<T>(Weak, Id);
			Events.OnClosed = [Weak, Id]() {
				RunOnGameThread(Weak, [Id](T* Self) {
					if (StreamClosed(Self->Net, Id)) {
						Self->OnClose.Broadcast();
						NotifyClosed(Self);
					}
				});
			};
			return Events;
		}

		template <typename T>
		static std::function<void(const asio::error_code&)> MakeErrorEvent(const TWeakObjectPtr<T>& Weak, uint64 Id) {
			return [Weak, Id](const asio::error_code& Ec) {
				RunOnGameThread(Weak, [Id, Ec](T* Self) {
					if (Self->Net.SessionId == Id || Self->Net.PendingCloseId == Id) {
						Self->Net.LastError = FErrorCode(Ec);
						Self->OnError.Broadcast(Self->Net.LastError);
					}
				});
			};
		}

		/** Updates state for a closed session; returns true if OnClose must be broadcast. */
		template <typename TState>
		static bool StreamClosed(TState& State, uint64 Id) {
			if (State.SessionId == Id) {
				State.SessionId = 0;
				State.bOpen = false;
				ResetNative(State);
				return true;
			}
			if (State.PendingCloseId == Id) {
				State.PendingCloseId = 0;
				return true;
			}
			return false;
		}

		static void ResetNative(FIpStreamState& State) { State.Transport.reset(); }
		static void ResetNative(FIpServerState& State) { State.Acceptor.reset(); }
		static void ResetNative(FIpUdpState& State) { State.Socket.reset(); }

		template <typename T>
		static auto NotifyClosed(T* Self) -> decltype(Self->HandleClosedNative(), void()) { Self->HandleClosedNative(); }
		static void NotifyClosed(...) {}

		template <typename T>
		static void FailImmediately(T* Self, EError Error) {
			Self->Net.LastError = FErrorCode(MakeError(Error));
			Self->OnError.Broadcast(Self->Net.LastError);
		}

		template <typename T>
		static bool ConnectTcp(T* Self, const FClientBindOptions& Options, const std::shared_ptr<const FTlsConfig>& Tls, bool bUseTls) {
			if (Self->Net.Transport) {
				return false;
			}
			const uint64 Id = NextSessionId();
			FTransportEvents Events = MakeTcpEvents(Self, Id);
			std::shared_ptr<FTransport> Transport = bUseTls ? MakeTlsClient(Tls, MoveTemp(Events)) : MakeTcpClient(MoveTemp(Events));
			if (!Transport) {
				FailImmediately(Self, EError::NotRunning);
				return false;
			}
			BeginSession(Self->Net, Transport, Id);
			Transport->Connect(ToStd(Options.Address), ToStd(Options.Port), Options.Protocol);
			return true;
		}

		static void BeginSession(FIpStreamState& State, const std::shared_ptr<FTransport>& Transport, uint64 Id) {
			State.Transport = Transport;
			State.SessionId = Id;
			State.bOpen = false;
			State.Local = FTcpEndpoint();
			State.Remote = FTcpEndpoint();
			State.LastError = FErrorCode();
		}

		/** User-initiated close: stops delivering events of this session except its final OnClose. */
		static void CloseStream(FIpStreamState& State) {
			if (!State.Transport) {
				return;
			}
			State.Transport->Close();
			State.Transport.reset();
			State.PendingCloseId = State.SessionId;
			State.SessionId = 0;
			State.bOpen = false;
		}

		/** Called from BeginDestroy: no event can be delivered anymore, just release the native session. */
		static void ReleaseStream(FIpStreamState& State) {
			if (State.Transport) {
				State.Transport->Close();
				State.Transport.reset();
			}
			State.SessionId = 0;
			State.PendingCloseId = 0;
		}

		template <typename T, typename TDelegate>
		static bool SendBytes(T* Self, FBytes Bytes, const TDelegate& Callback) {
			if (!Self->Net.Transport || !Self->Net.bOpen || Bytes.empty()) {
				return false;
			}
			return Self->Net.Transport->Send(MoveTemp(Bytes), MakeSendCallback(Self, Callback));
		}

		// ------------------------------------------------------------------------------------------------
		// Servers. Required members: FIpServerState Server; OnError(Error); OnClose()

		/**
		 * Opens the listening socket synchronously (bind errors are reported immediately, as before) and
		 * starts accepting. OnAccept runs on the Game Thread with a socket that must be adopted or dropped.
		 */
		template <typename T>
		static bool OpenServer(T* Self, const FServerBindOptions& Options, TFunction<void(T*, asio::ip::tcp::socket&&)> OnAccept) {
			if (Self->Server.Acceptor) {
				return false;
			}
			const uint64 Id = NextSessionId();
			TWeakObjectPtr<T> Weak(Self);
			FAcceptorEvents Events;
			TSharedRef<TFunction<void(T*, asio::ip::tcp::socket&&)>, ESPMode::ThreadSafe> Handler =
				MakeShared<TFunction<void(T*, asio::ip::tcp::socket&&)>, ESPMode::ThreadSafe>(MoveTemp(OnAccept));
			Events.OnAccept = [Weak, Id, Handler](asio::ip::tcp::socket&& Socket) {
				std::shared_ptr<asio::ip::tcp::socket> Owned = std::make_shared<asio::ip::tcp::socket>(MoveTemp(Socket));
				RunOnGameThread(Weak, [Id, Handler, Owned](T* Self) {
					if (Self->Server.SessionId != Id || !Self->Server.bOpen) {
						asio::error_code Ignored;
						Owned->close(Ignored);
						return;
					}
					(*Handler)(Self, MoveTemp(*Owned));
				});
			};
			Events.OnError = [Weak, Id](const asio::error_code& Ec) {
				RunOnGameThread(Weak, [Id, Ec](T* Self) {
					if (Self->Server.SessionId == Id) {
						Self->Server.LastError = FErrorCode(Ec);
						Self->OnError.Broadcast(Self->Server.LastError);
					}
				});
			};
			Events.OnClosed = [Weak, Id]() {
				RunOnGameThread(Weak, [Id](T* Self) {
					if (StreamClosed(Self->Server, Id)) {
						Self->OnClose.Broadcast();
					}
				});
			};

			std::shared_ptr<FTcpAcceptor> Acceptor = FTcpAcceptor::Create(MoveTemp(Events));
			if (!Acceptor) {
				Self->Server.LastError = FErrorCode(MakeError(EError::NotRunning));
				Self->OnError.Broadcast(Self->Server.LastError);
				return false;
			}

			asio::ip::address Address;
			if (!MakeListenAddress(Options.Address, Options.Protocol, Address)) {
				Acceptor->Close();
				Self->Server.LastError = FErrorCode(MakeError(EError::InvalidArgument));
				Self->OnError.Broadcast(Self->Server.LastError);
				return false;
			}
			if (Options.Port < 0 || Options.Port > 65535) {
				Acceptor->Close();
				Self->Server.LastError = FErrorCode(MakeError(EError::InvalidArgument));
				Self->OnError.Broadcast(Self->Server.LastError);
				return false;
			}

			asio::error_code Ec;
			if (!Acceptor->Open(asio::ip::tcp::endpoint(Address, static_cast<uint16>(Options.Port)), Options.bReuse_Address, Self->Backlog, Ec)) {
				Acceptor->Close();
				Self->Server.LastError = FErrorCode(Ec);
				Self->OnError.Broadcast(Self->Server.LastError);
				return false;
			}

			Self->Server.Acceptor = Acceptor;
			Self->Server.SessionId = Id;
			Self->Server.bOpen = true;
			Self->Server.Local = FTcpEndpoint(Acceptor->LocalEndpoint());
			Self->Server.LastError = FErrorCode();
			Acceptor->Start();
			return true;
		}

		/** Stops listening. Clients are closed by the caller. */
		template <typename T>
		static void CloseServer(T* Self) {
			if (!Self->Server.Acceptor) {
				return;
			}
			Self->Server.Acceptor->Close();
			Self->Server.Acceptor.reset();
			Self->Server.PendingCloseId = Self->Server.SessionId;
			Self->Server.SessionId = 0;
			Self->Server.bOpen = false;
		}

		static void ReleaseServer(FIpServerState& State) {
			if (State.Acceptor) {
				State.Acceptor->Close();
				State.Acceptor.reset();
			}
			State.SessionId = 0;
			State.PendingCloseId = 0;
			State.bOpen = false;
		}

		/** Creates a remote facade owned by the server and adopts an accepted TCP/TLS connection. */
		template <typename TServer, typename TRemote>
		static TRemote* AdoptTcpRemote(TServer* Server, TSet<TRemote*>& Clients, asio::ip::tcp::socket&& Socket,
		                               const std::shared_ptr<const FTlsConfig>& Tls, bool bUseTls) {
			TRemote* Remote = NewObject<TRemote>(Server);
			const uint64 Id = NextSessionId();
			FTransportEvents Events = MakeTcpEvents(Remote, Id);
			std::shared_ptr<FTransport> Transport = bUseTls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events))
			                                                : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events));
			if (!Transport) {
				return nullptr;
			}
			BeginSession(Remote->Net, Transport, Id);
			BindRemoteToServer(Server, Clients, Remote);
			return Remote;
		}

		// ------------------------------------------------------------------------------------------------
		// UDP (UUDPClient, UUDPServer). Required members: FIpUdpState Net; OnMessage; OnClose(); OnError(Error)

		template <typename T, typename FDataHandler>
		static FUdpEvents MakeUdpEvents(T* Owner, uint64 Id, FDataHandler DataHandler) {
			TWeakObjectPtr<T> Weak(Owner);
			FUdpEvents Events;
			Events.OnConnected = [Weak, Id](const asio::ip::udp::endpoint& Local, const asio::ip::udp::endpoint& Remote) {
				RunOnGameThread(Weak, [Id, Local, Remote](T* Self) {
					if (Self->Net.SessionId != Id) {
						return;
					}
					Self->Net.bOpen = true;
					Self->Net.Local = FUdpEndpoint(Local);
					Self->Net.Remote = FUdpEndpoint(Remote);
					BroadcastConnected(Self, 0);
				});
			};
			Events.OnData = [Weak, Id, DataHandler](const uint8* Data, size_t Size, const asio::ip::udp::endpoint& From) {
				TArray<uint8> Buffer(Data, static_cast<int32>(Size));
				RunOnGameThread(Weak, [Id, From, DataHandler, Buffer = MoveTemp(Buffer)](T* Self) {
					if (Self->Net.SessionId == Id) {
						DataHandler(Self, Buffer, From);
					}
				});
			};
			Events.OnError = MakeErrorEvent<T>(Weak, Id);
			Events.OnClosed = [Weak, Id]() {
				RunOnGameThread(Weak, [Id](T* Self) {
					if (StreamClosed(Self->Net, Id)) {
						Self->OnClose.Broadcast();
					}
				});
			};
			return Events;
		}

		/** UUDPClient has OnConnected, UUDPServer does not (it reports OnListening synchronously from Bind). */
		template <typename T>
		static auto BroadcastConnected(T* Self, int) -> decltype(Self->OnConnected.Broadcast(), void()) { Self->OnConnected.Broadcast(); }
		template <typename T>
		static void BroadcastConnected(T*, long) {}

		static void CloseUdp(FIpUdpState& State) {
			if (!State.Socket) {
				return;
			}
			State.Socket->Close();
			State.Socket.reset();
			State.PendingCloseId = State.SessionId;
			State.SessionId = 0;
			State.bOpen = false;
		}

		static void ReleaseUdp(FIpUdpState& State) {
			if (State.Socket) {
				State.Socket->Close();
				State.Socket.reset();
			}
			State.SessionId = 0;
			State.PendingCloseId = 0;
			State.bOpen = false;
		}

		template <typename TRemote>
		static void StartRemote(TRemote* Remote) {
			if (Remote->Net.Transport) {
				Remote->Net.Transport->StartAccepted();
			}
		}

		/** The server keeps its remotes alive through a UPROPERTY set and forgets them when they close. */
		template <typename TServer, typename TRemote>
		static void BindRemoteToServer(TServer* Server, TSet<TRemote*>& Clients, TRemote* Remote) {
			Clients.Add(Remote);
			TWeakObjectPtr<TServer> WeakServer(Server);
			TWeakObjectPtr<TRemote> WeakRemote(Remote);
			Remote->OnClosedNative = [WeakServer, WeakRemote]() {
				TServer* ServerPtr = WeakServer.Get();
				TRemote* RemotePtr = WeakRemote.Get();
				if (ServerPtr && RemotePtr) {
					ServerPtr->RemoveClient(RemotePtr);
				}
			};
		}
	};
}
