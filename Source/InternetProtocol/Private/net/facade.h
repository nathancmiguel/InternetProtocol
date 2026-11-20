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
#include <algorithm>
#include <atomic>

/*
 * Game Thread helpers shared by the UObject facades. Every function here runs on the Game Thread except
 * the lambdas stored in FTransportEvents / FAcceptorEvents, which run on a network strand and only copy
 * data and post it to the Game Thread with RunOnGameThread (weak UObject + session id).
 */
namespace IPNet {

	/**
	 * Moves with a Game Thread task and consumes its bytes from the flow control when the task is destroyed,
	 * i.e. after it ran or when it was dropped because the UObject is gone.
	 */
	struct FFlowGuard {
		FFlowGuard(std::shared_ptr<FFlowControl> InFlow, size_t InBytes) : Flow(MoveTemp(InFlow)), Bytes(static_cast<int64>(InBytes)) {}
		FFlowGuard(FFlowGuard&& Other) noexcept : Flow(MoveTemp(Other.Flow)), Bytes(Other.Bytes) { Other.Flow.reset(); }
		FFlowGuard(const FFlowGuard& Other) : Flow(Other.Flow), Bytes(Other.Bytes) { const_cast<FFlowGuard&>(Other).Flow.reset(); }
		FFlowGuard& operator=(const FFlowGuard&) = delete;
		~FFlowGuard() {
			if (Flow) {
				Flow->Consume(Bytes);
			}
		}
		std::shared_ptr<FFlowControl> Flow;
		int64 Bytes = 0;
	};

	/** Cuts a TCP byte stream into messages (strand only). */
	class FTcpFramer {
	public:
		explicit FTcpFramer(const FTcpFramingOptions& Options)
			: Mode(Options.Mode), MaxBytes(static_cast<size_t>(FMath::Max(Options.MaxMessageBytes, 1))) {
			const FTCHARToUTF8 Utf8(*Options.Delimiter);
			Delimiter.assign(reinterpret_cast<const uint8*>(Utf8.Get()), reinterpret_cast<const uint8*>(Utf8.Get()) + Utf8.Length());
			if (Mode == ETcpFramingMode::Delimiter && Delimiter.empty()) {
				Delimiter.push_back('\n');
			}
		}

		/** Appends data and calls Emit for every complete message. Returns false on a framing error. */
		template <typename FEmit>
		bool Feed(const uint8* Data, size_t Size, FEmit&& Emit) {
			Buffer.insert(Buffer.end(), Data, Data + Size);
			size_t Pos = 0;
			bool bOk = true;
			for (;;) {
				const size_t Available = Buffer.size() - Pos;
				if (Mode == ETcpFramingMode::LengthPrefixed) {
					if (Available < 4) {
						break;
					}
					const uint8* P = Buffer.data() + Pos;
					const size_t Length = (static_cast<size_t>(P[0]) << 24) | (static_cast<size_t>(P[1]) << 16) | (static_cast<size_t>(P[2]) << 8) | P[3];
					if (Length > MaxBytes) {
						bOk = false;
						break;
					}
					if (Available < 4 + Length) {
						break;
					}
					Emit(P + 4, Length);
					Pos += 4 + Length;
				} else {
					const auto Begin = Buffer.begin() + static_cast<std::ptrdiff_t>(Pos);
					const auto Found = std::search(Begin, Buffer.end(), Delimiter.begin(), Delimiter.end());
					if (Found == Buffer.end()) {
						if (Available > MaxBytes) {
							bOk = false;
						}
						break;
					}
					const size_t Length = static_cast<size_t>(Found - Begin);
					if (Length > MaxBytes) {
						bOk = false;
						break;
					}
					Emit(Buffer.data() + Pos, Length);
					Pos += Length + Delimiter.size();
				}
			}
			Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos));
			return bOk;
		}

		/** Frames one outgoing message; false if it is too large. */
		static bool Encode(const FTcpFramingOptions& Options, FBytes& InOut) {
			if (Options.Mode == ETcpFramingMode::Raw) {
				return true;
			}
			if (InOut.size() > static_cast<size_t>(FMath::Max(Options.MaxMessageBytes, 1))) {
				return false;
			}
			if (Options.Mode == ETcpFramingMode::LengthPrefixed) {
				const uint32 Length = static_cast<uint32>(InOut.size());
				const uint8 Prefix[4] = {static_cast<uint8>(Length >> 24), static_cast<uint8>(Length >> 16), static_cast<uint8>(Length >> 8), static_cast<uint8>(Length)};
				InOut.insert(InOut.begin(), Prefix, Prefix + 4);
				return true;
			}
			const FTCHARToUTF8 Utf8(*(Options.Delimiter.IsEmpty() ? FString(TEXT("\n")) : Options.Delimiter));
			InOut.insert(InOut.end(), reinterpret_cast<const uint8*>(Utf8.Get()), reinterpret_cast<const uint8*>(Utf8.Get()) + Utf8.Length());
			return true;
		}

	private:
		ETcpFramingMode Mode;
		size_t MaxBytes;
		FBytes Delimiter;
		FBytes Buffer;
	};

	inline uint64 NextSessionId() {
		static std::atomic<uint64> Counter{0};
		return ++Counter;
	}

	inline std::string ToStd(const FString& Text) {
		return std::string(TCHAR_TO_UTF8(*Text));
	}

	inline std::chrono::milliseconds ToMs(float Seconds) {
		return std::chrono::milliseconds(Seconds > 0.0f ? static_cast<int64>(Seconds * 1000.0f) : 0);
	}

	inline std::shared_ptr<FFlowControl> MakeFlow(int32 MaxPendingMB) {
		return std::make_shared<FFlowControl>(static_cast<int64>(FMath::Max(MaxPendingMB, 1)) * 1024 * 1024);
	}

	/** Transport options of an outgoing connection. */
	inline FTransportOptions ClientTransportOptions(const FClientBindOptions& Bind, bool bFlow) {
		FTransportOptions Options;
		Options.ConnectTimeout = ToMs(Bind.ConnectTimeoutSeconds);
		Options.bNoDelay = Bind.bNoDelay;
		Options.bKeepAlive = Bind.bKeepAlive;
		Options.MaxQueuedBytes = static_cast<size_t>(FMath::Max(Bind.MaxQueuedMB, 1)) * 1024 * 1024;
		if (bFlow) {
			Options.Flow = MakeFlow(Bind.MaxPendingReceiveMB);
		}
		return Options;
	}

	/** Transport options of an accepted connection. bEstablishOnOpen = false when a protocol handshake follows. */
	inline FTransportOptions AcceptedTransportOptions(const FServerBindOptions& Bind, bool bFlow, bool bEstablishOnOpen) {
		FTransportOptions Options;
		Options.HandshakeTimeout = ToMs(Bind.HandshakeTimeoutSeconds);
		Options.bNoDelay = Bind.bNoDelay;
		Options.bKeepAlive = Bind.bKeepAlive;
		Options.MaxQueuedBytes = static_cast<size_t>(FMath::Max(Bind.MaxQueuedMB, 1)) * 1024 * 1024;
		Options.bEstablishOnOpen = bEstablishOnOpen;
		if (bFlow) {
			Options.Flow = MakeFlow(Bind.MaxPendingReceiveMB);
		}
		return Options;
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
		static FTransportEvents MakeTcpEvents(T* Owner, uint64 Id, std::shared_ptr<FFlowControl> Flow = nullptr,
		                                      const FTcpFramingOptions& Framing = FTcpFramingOptions()) {
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
			std::shared_ptr<FTcpFramer> Framer = Framing.Mode == ETcpFramingMode::Raw ? nullptr : std::make_shared<FTcpFramer>(Framing);
			Events.OnData = [Weak, Id, Flow, Framer](const uint8* Data, size_t Size) {
				if (Framer) {
					// One OnMessage per complete message.
					const bool bOk = Framer->Feed(Data, Size, [&Weak, Id, &Flow](const uint8* Message, size_t Length) {
						TArray<uint8> Buffer(Message, static_cast<int32>(Length));
						if (Flow) {
							Flow->Add(static_cast<int64>(Length));
						}
						RunOnGameThread(Weak, [Id, Guard = FFlowGuard(Flow, Length), Buffer = MoveTemp(Buffer)](T* Self) {
							if (Self->Net.SessionId == Id) {
								++Self->Net.MessagesReceived;
								Self->OnMessage.Broadcast(Buffer, Buffer.Num());
							}
						});
					});
					if (!bOk) {
						RunOnGameThread(Weak, [Id](T* Self) {
							if (Self->Net.SessionId == Id) {
								FailImmediately(Self, EError::MessageTooLarge);
								CloseStream(Self->Net);
							}
						});
					}
					return;
				}
				TArray<uint8> Buffer(Data, static_cast<int32>(Size));
				if (Flow) {
					Flow->Add(static_cast<int64>(Size));
				}
				// The Consume runs even when the object is gone (FFlowGuard), so reading can never stay paused.
				RunOnGameThread(Weak, [Id, Guard = FFlowGuard(Flow, Size), Buffer = MoveTemp(Buffer)](T* Self) {
					if (Self->Net.SessionId == Id) {
						++Self->Net.MessagesReceived;
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
					if (Self->Net.SessionId == Id || Self->Net.PendingCloseIds.Contains(Id)) {
						Self->Net.LastError = FErrorCode(Ec);
						Self->OnError.Broadcast(Self->Net.LastError);
					}
				});
			};
		}

		enum class EClosed : uint8 {
			/** Not ours (stale event): ignore. */
			None,
			/** The current session closed: its state must be cleared and OnClose broadcast. */
			Current,
			/** A session the user already closed (maybe followed by a new Connect): only OnClose is broadcast. */
			Pending,
		};

		/**
		 * Updates state for a closed session. Only the current session clears the shared state: after
		 * Close() + Connect(), the old session's late close must not wipe the new one.
		 */
		template <typename TState>
		static EClosed StreamClosedKind(TState& State, uint64 Id) {
			if (Id != 0 && State.SessionId == Id) {
				State.SessionId = 0;
				State.bOpen = false;
				ResetNative(State);
				return EClosed::Current;
			}
			if (State.PendingCloseIds.Remove(Id) > 0) {
				return EClosed::Pending;
			}
			return EClosed::None;
		}

		/** Returns true if OnClose must be broadcast. */
		template <typename TState>
		static bool StreamClosed(TState& State, uint64 Id) {
			return StreamClosedKind(State, Id) != EClosed::None;
		}

		/** Remembers a user-closed session so that its final OnClose is still delivered. */
		template <typename TState>
		static void AddPendingClose(TState& State, uint64 Id) {
			if (Id == 0) {
				return;
			}
			// Bounded: a session whose close never arrives (should not happen) cannot grow this forever.
			if (State.PendingCloseIds.Num() >= 16) {
				State.PendingCloseIds.RemoveAt(0);
			}
			State.PendingCloseIds.AddUnique(Id);
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
			FTransportOptions TransportOptions = ClientTransportOptions(Options, true);
			FTransportEvents Events = MakeTcpEvents(Self, Id, TransportOptions.Flow, Self->Framing);
			std::shared_ptr<FTransport> Transport = bUseTls ? MakeTlsClient(Tls, MoveTemp(Events), TransportOptions)
			                                                : MakeTcpClient(MoveTemp(Events), TransportOptions);
			if (!Transport) {
				FailImmediately(Self, EError::NotRunning);
				return false;
			}
			BeginSession(Self->Net, Transport, Id);
			Self->Net.Framing = Self->Framing;
			Transport->Connect(ToStd(Options.Address), ToStd(Options.Port), Options.Protocol);
			return true;
		}

		static void BeginSession(FIpStreamState& State, const std::shared_ptr<FTransport>& Transport, uint64 Id) {
			State.Transport = Transport;
			State.MessagesSent = 0;
			State.MessagesReceived = 0;
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
			AddPendingClose(State, State.SessionId);
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
			State.PendingCloseIds.Reset();
		}

		template <typename T, typename TDelegate>
		static bool SendBytes(T* Self, FBytes Bytes, const TDelegate& Callback) {
			if (!Self->Net.Transport || !Self->Net.bOpen || (Bytes.empty() && Self->Net.Framing.Mode == ETcpFramingMode::Raw)) {
				return false;
			}
			if (!FTcpFramer::Encode(Self->Net.Framing, Bytes)) {
				FailImmediately(Self, EError::MessageTooLarge);
				return false;
			}
			if (!Self->Net.Transport->Send(MoveTemp(Bytes), MakeSendCallback(Self, Callback))) {
				// The send queue is full (the peer does not read fast enough).
				FailImmediately(Self, EError::QueueFull);
				return false;
			}
			++Self->Net.MessagesSent;
			return true;
		}

		/** Counters of the current connection (bytes from the transport, messages from the facade). */
		static FConnectionStats StreamStats(const FIpStreamState& State) {
			FConnectionStats Stats;
			if (State.Transport) {
				const FTransportStats Native = State.Transport->GetStats();
				Stats.BytesSent = static_cast<int64>(Native.BytesSent);
				Stats.BytesReceived = static_cast<int64>(Native.BytesReceived);
				Stats.QueuedBytes = static_cast<int64>(Native.QueuedBytes);
			}
			Stats.MessagesSent = State.MessagesSent;
			Stats.MessagesReceived = State.MessagesReceived;
			return Stats;
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
					if (Self->Server.Options.MaxConnections > 0 && Self->Clients().Num() >= Self->Server.Options.MaxConnections) {
						static double LastLog = 0.0;
						if (FPlatformTime::Seconds() - LastLog >= 1.0) {
							LastLog = FPlatformTime::Seconds();
							UE_LOG(LogAsio, Warning, TEXT("InternetProtocol: connection refused, MaxConnections (%d) reached."), Self->Server.Options.MaxConnections);
						}
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
			Self->Server.Options = Options;
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
			AddPendingClose(Self->Server, Self->Server.SessionId);
			Self->Server.SessionId = 0;
			Self->Server.bOpen = false;
		}

		static void ReleaseServer(FIpServerState& State) {
			if (State.Acceptor) {
				State.Acceptor->Close();
				State.Acceptor.reset();
			}
			State.SessionId = 0;
			State.PendingCloseIds.Reset();
			State.bOpen = false;
		}

		/** Creates a remote facade owned by the server and adopts an accepted TCP/TLS connection. */
		template <typename TServer, typename TRemote>
		static TRemote* AdoptTcpRemote(TServer* Server, TSet<TObjectPtr<TRemote>>& Clients, asio::ip::tcp::socket&& Socket,
		                               const std::shared_ptr<const FTlsConfig>& Tls, bool bUseTls) {
			TRemote* Remote = NewObject<TRemote>(Server);
			const uint64 Id = NextSessionId();
			FTransportOptions TransportOptions = AcceptedTransportOptions(Server->Server.Options, true, true);
			FTransportEvents Events = MakeTcpEvents(Remote, Id, TransportOptions.Flow, Server->Framing);
			std::shared_ptr<FTransport> Transport = bUseTls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events), TransportOptions)
			                                                : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events), TransportOptions);
			if (!Transport) {
				return nullptr;
			}
			BeginSession(Remote->Net, Transport, Id);
			Remote->Net.Framing = Server->Framing;
			BindRemoteToServer(Server, Clients, Remote);
			return Remote;
		}

		// ------------------------------------------------------------------------------------------------
		// UDP (UUDPClient, UUDPServer). Required members: FIpUdpState Net; OnMessage; OnClose(); OnError(Error)

		template <typename T, typename FDataHandler>
		static FUdpEvents MakeUdpEvents(T* Owner, uint64 Id, FDataHandler DataHandler, std::shared_ptr<FFlowControl> Flow = nullptr) {
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
			Events.OnData = [Weak, Id, DataHandler, Flow](const uint8* Data, size_t Size, const asio::ip::udp::endpoint& From) {
				TArray<uint8> Buffer(Data, static_cast<int32>(Size));
				// The socket already counted the datagram in Flow; the guard releases it on the Game Thread.
				RunOnGameThread(Weak, [Id, From, DataHandler, Guard = FFlowGuard(Flow, Size), Buffer = MoveTemp(Buffer)](T* Self) {
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
			AddPendingClose(State, State.SessionId);
			State.SessionId = 0;
			State.bOpen = false;
		}

		static void ReleaseUdp(FIpUdpState& State) {
			if (State.Socket) {
				State.Socket->Close();
				State.Socket.reset();
			}
			State.SessionId = 0;
			State.PendingCloseIds.Reset();
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
		static void BindRemoteToServer(TServer* Server, TSet<TObjectPtr<TRemote>>& Clients, TRemote* Remote) {
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
