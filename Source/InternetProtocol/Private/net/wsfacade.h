/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/facade.h"
#include "net/wssession.h"

namespace IPNet {

	/**
	 * WebSocket facades (UWSClient, UWSClientSsl, UWSRemote, UWSRemoteSsl).
	 * Required members: FIpWsState State; OnConnected(Handshake); OnUnexpectedHandshake(Handshake);
	 * OnMessage(Buffer, bIsBinary); OnPing(); OnPong(); OnClose(Code, Reason); OnError(Error).
	 * Remotes also have TFunction<void()> OnClosedNative.
	 */
	struct FWsAccess {

		/** Set once the transport exists; lets session callbacks (strand) reach it. */
		using FTransportRef = std::shared_ptr<std::weak_ptr<FTransport>>;

		static void MarkEstablished(const FTransportRef& Ref) {
			if (std::shared_ptr<FTransport> Strong = Ref->lock()) {
				Strong->MarkEstablished();
			}
		}

		template <typename T>
		static FWsSession::FCallbacks MakeCommonCallbacks(const TWeakObjectPtr<T>& Weak, uint64 Id, std::shared_ptr<FFlowControl> Flow = nullptr) {
			FWsSession::FCallbacks Callbacks;
			Callbacks.OnMessage = [Weak, Id, Flow](FBytes&& Payload, bool bBinary) {
				TArray<uint8> Buffer(Payload.data(), static_cast<int32>(Payload.size()));
				if (Flow) {
					Flow->Add(static_cast<int64>(Payload.size()));
				}
				RunOnGameThread(Weak, [Id, bBinary, Guard = FFlowGuard(Flow, Payload.size()), Buffer = MoveTemp(Buffer)](T* Self) {
					if (Self->State.Net.SessionId == Id) {
						Self->OnMessage.Broadcast(Buffer, bBinary);
					}
				});
			};
			Callbacks.OnPing = [Weak, Id]() {
				RunOnGameThread(Weak, [Id](T* Self) {
					if (Self->State.Net.SessionId == Id) {
						Self->OnPing.Broadcast();
					}
				});
			};
			Callbacks.OnPong = [Weak, Id]() {
				RunOnGameThread(Weak, [Id](T* Self) {
					if (Self->State.Net.SessionId == Id) {
						if (Self->State.LastPingSeconds > 0.0) {
							Self->State.RoundTripMs = static_cast<float>((FPlatformTime::Seconds() - Self->State.LastPingSeconds) * 1000.0);
							Self->State.LastPingSeconds = 0.0;
						}
						Self->OnPong.Broadcast();
					}
				});
			};
			return Callbacks;
		}

		template <typename T>
		static FTransportEvents MakeTransportEvents(const TWeakObjectPtr<T>& Weak, uint64 Id, const std::shared_ptr<FWsSession>& Session) {
			FTransportEvents Events;
			Events.OnConnected = [Weak, Id, Session](const asio::ip::tcp::endpoint& Local, const asio::ip::tcp::endpoint& Remote) {
				Session->OnTransportConnected();
				RunOnGameThread(Weak, [Id, Local, Remote](T* Self) {
					if (Self->State.Net.SessionId == Id) {
						Self->State.Net.Local = FTcpEndpoint(Local);
						Self->State.Net.Remote = FTcpEndpoint(Remote);
					}
				});
			};
			Events.OnData = [Session](const uint8* Data, size_t Size) { Session->OnData(Data, Size); };
			Events.OnError = [Weak, Id](const asio::error_code& Ec) {
				RunOnGameThread(Weak, [Id, Ec](T* Self) {
					if (Self->State.Net.SessionId == Id || Self->State.Net.PendingCloseIds.Contains(Id)) {
						Self->State.Net.LastError = FErrorCode(Ec);
						Self->OnError.Broadcast(Self->State.Net.LastError);
					}
				});
			};
			Events.OnClosed = [Weak, Id, Session]() {
				uint16 Code = 1006;
				FString Reason;
				Session->OnTransportClosed(Code, Reason);
				RunOnGameThread(Weak, [Id, Code, Reason](T* Self) {
					const FFacadeAccess::EClosed Kind = FFacadeAccess::StreamClosedKind(Self->State.Net, Id);
					if (Kind != FFacadeAccess::EClosed::None) {
						// After Close() + Connect() the WebSocket session belongs to the new connection.
						if (Kind == FFacadeAccess::EClosed::Current) {
							Self->State.Session.reset();
							Self->State.bClosing = false;
						}
						Self->OnClose.Broadcast(Code, Reason);
						FFacadeAccess::NotifyClosed(Self);
					}
				});
			};
			return Events;
		}

		// ---- client ----

		static FString HostHeader(const FClientBindOptions& Options, bool bTls) {
			FString Host = Options.Address;
			if (Host.Contains(TEXT(":")) && !Host.StartsWith(TEXT("["))) {
				Host = TEXT("[") + Host + TEXT("]");
			}
			const int32 DefaultPort = bTls ? 443 : 80;
			if (!Options.Port.IsEmpty() && FCString::Atoi(*Options.Port) != DefaultPort) {
				Host += TEXT(":") + Options.Port;
			}
			return Host;
		}

		template <typename T>
		static bool Connect(T* Self, const FClientBindOptions& Options, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			FIpWsState& S = Self->State;
			if (S.Net.Transport) {
				return false;
			}
			const uint64 Id = NextSessionId();
			TWeakObjectPtr<T> Weak(Self);

			// Upgrade request: user headers/path are kept, the protocol headers are always ours.
			FHttpRequest Request = Self->Handshake;
			Request.Method = ERequestMethod::GET;
			Request.Version = TEXT("1.1");
			Request.Body.Reset();
			// Requested subprotocols (user header), validated against the server's choice.
			FWsSession::FOptions SessionOptions;
			if (const FString* Protocols = FindHeader(Request.Headers, TEXT("Sec-WebSocket-Protocol"))) {
				SessionOptions.RequestedProtocols = SplitHeaderList(*Protocols);
			}
			// No extension is implemented: users cannot negotiate one (its framing would not be understood).
			for (const TCHAR* Name : {TEXT("Upgrade"), TEXT("Connection"), TEXT("Sec-WebSocket-Key"), TEXT("Sec-WebSocket-Version"), TEXT("Sec-WebSocket-Extensions")}) {
				TArray<FString> Keys;
				Request.Headers.GetKeys(Keys);
				for (const FString& Existing : Keys) {
					if (Existing.Equals(Name, ESearchCase::IgnoreCase)) {
						Request.Headers.Remove(Existing);
					}
				}
			}
			const FString Key = GenerateWebSocketKey();
			Request.Headers.Add(TEXT("Upgrade"), TEXT("websocket"));
			Request.Headers.Add(TEXT("Connection"), TEXT("Upgrade"));
			Request.Headers.Add(TEXT("Sec-WebSocket-Key"), Key);
			Request.Headers.Add(TEXT("Sec-WebSocket-Version"), TEXT("13"));
			FBytes HandshakeBytes = SerializeRequest(Request, HostHeader(Options, bTls));

			// The connect timeout also covers the upgrade response.
			FTransportOptions TransportOptions = ClientTransportOptions(Options, true);
			TransportOptions.bEstablishOnOpen = false;
			FTransportRef TransportRef = std::make_shared<std::weak_ptr<FTransport>>();
			FWsSession::FCallbacks Callbacks = MakeCommonCallbacks(Weak, Id, TransportOptions.Flow);
			Callbacks.OnClientHandshake = [Weak, Id, TransportRef](FHttpResponse&& Response) {
				MarkEstablished(TransportRef);
				RunOnGameThread(Weak, [Id, Response = MoveTemp(Response)](T* Owner) {
					if (Owner->State.Net.SessionId == Id) {
						Owner->State.Net.bOpen = true;
						const FString* Protocol = FindHeader(Response.Headers, TEXT("Sec-WebSocket-Protocol"));
						Owner->State.SubProtocol = Protocol ? Protocol->TrimStartAndEnd() : FString();
						Owner->OnConnected.Broadcast(Response);
					}
				});
			};
			Callbacks.OnClientUnexpected = [Weak, Id](FHttpResponse&& Response) {
				RunOnGameThread(Weak, [Id, Response = MoveTemp(Response)](T* Owner) {
					if (Owner->State.Net.SessionId == Id) {
						Owner->OnUnexpectedHandshake.Broadcast(Response);
					}
				});
			};
			std::shared_ptr<FWsSession> Session = std::make_shared<FWsSession>(true, MoveTemp(Callbacks), MoveTemp(HandshakeBytes), Key, MoveTemp(SessionOptions));

			FTransportEvents Events = MakeTransportEvents(Weak, Id, Session);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsClient(Tls, MoveTemp(Events), TransportOptions)
			                                             : MakeTcpClient(MoveTemp(Events), TransportOptions);
			if (Transport) {
				*TransportRef = Transport;
			}
			if (!Transport) {
				S.Net.LastError = FErrorCode(MakeError(EError::NotRunning));
				Self->OnError.Broadcast(S.Net.LastError);
				return false;
			}
			Session->Transport = Transport;
			FFacadeAccess::BeginSession(S.Net, Transport, Id);
			S.Session = Session;
			S.bClosing = false;
			Transport->Connect(ToStd(Options.Address), ToStd(Options.Port), Options.Protocol);
			return true;
		}

		// ---- server side remote ----

		template <typename TServer, typename TRemote>
		static TRemote* AdoptRemote(TServer* Server, TSet<TRemote*>& Clients, asio::ip::tcp::socket&& Socket,
		                            const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			TRemote* Remote = NewObject<TRemote>(Server);
			const uint64 Id = NextSessionId();
			TWeakObjectPtr<TRemote> Weak(Remote);

			// TLS + upgrade request must arrive within the handshake timeout.
			FTransportOptions TransportOptions = AcceptedTransportOptions(Server->Server.Options, true, false);
			FTransportRef TransportRef = std::make_shared<std::weak_ptr<FTransport>>();
			FWsSession::FCallbacks Callbacks = MakeCommonCallbacks(Weak, Id, TransportOptions.Flow);
			Callbacks.OnServerHandshake = [Weak, Id, TransportRef](FHttpRequest&& Request) {
				MarkEstablished(TransportRef);
				RunOnGameThread(Weak, [Id, Request = MoveTemp(Request)](TRemote* Owner) mutable {
					if (Owner->State.Net.SessionId == Id) {
						Owner->State.Net.bOpen = true;
						FString Selected;
						Request.Headers.RemoveAndCopyValue(TEXT("x-selected-protocol"), Selected);
						Owner->State.SubProtocol = Selected;
						Owner->OnConnected.Broadcast(Request);
					}
				});
			};
			Callbacks.OnServerUnexpected = [Weak, Id](FHttpRequest&& Request) {
				RunOnGameThread(Weak, [Id, Request = MoveTemp(Request)](TRemote* Owner) {
					if (Owner->State.Net.SessionId == Id) {
						Owner->OnUnexpectedHandshake.Broadcast(Request);
					}
				});
			};
			FWsSession::FOptions SessionOptions;
			SessionOptions.ServerProtocols = Server->SubProtocols;
			SessionOptions.bManualAccept = Server->OnUpgradeRequest.IsBound();
			if (SessionOptions.bManualAccept) {
				TWeakObjectPtr<TServer> WeakServer(Server);
				Callbacks.OnUpgradeRequest = [Weak, WeakServer, Id](FHttpRequest&& Request) {
					RunOnGameThread(Weak, [Id, WeakServer, Request = MoveTemp(Request)](TRemote* Owner) {
						TServer* ServerPtr = WeakServer.Get();
						if (Owner->State.Net.SessionId != Id) {
							return;
						}
						if (!ServerPtr) {
							Owner->RejectUpgrade(503, TEXT("Server closed"));
							return;
						}
						ServerPtr->OnUpgradeRequest.Broadcast(Owner, Request);
					});
				};
			}
			std::shared_ptr<FWsSession> Session = std::make_shared<FWsSession>(false, MoveTemp(Callbacks), FBytes(), FString(), MoveTemp(SessionOptions));

			FTransportEvents Events = MakeTransportEvents(Weak, Id, Session);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events), TransportOptions)
			                                             : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events), TransportOptions);
			if (!Transport) {
				return nullptr;
			}
			*TransportRef = Transport;
			Session->Transport = Transport;
			FFacadeAccess::BeginSession(Remote->State.Net, Transport, Id);
			Remote->State.Session = Session;
			FFacadeAccess::BindRemoteToServer(Server, Clients, Remote);
			return Remote;
		}

		template <typename TRemote>
		static void StartRemote(TRemote* Remote) {
			if (Remote->State.Net.Transport) {
				Remote->State.Net.Transport->StartAccepted();
			}
		}

		// ---- shared operations ----

		template <typename T>
		static bool IsOpen(const T* Self) {
			return Self->State.Net.bOpen && !Self->State.bClosing && Self->State.Session != nullptr;
		}

		template <typename T, typename TDelegate>
		static bool SendFrame(T* Self, EOpcode Opcode, const uint8* Data, size_t Size, const FDataframe& Frame, const TDelegate& Callback) {
			if (!IsOpen(Self)) {
				return false;
			}
			const bool bMask = Self->State.Session->IsClient();
			const bool bControl = Opcode == EOpcode::PING || Opcode == EOpcode::PONG || Opcode == EOpcode::CLOSE_FRAME;
			// RSV bits are never sent: they only have a meaning with a negotiated extension.
			FBytes Bytes = EncodeWsFrame(Opcode, Data, Size, bControl ? true : Frame.bFin, false, false, false, bMask);
			Self->State.Session->SendFrame(MoveTemp(Bytes), MakeSendCallback(Self, Callback));
			return true;
		}

		/**
		 * Data message or fragment: Dataframe.bFin = false starts (or continues) a fragmented message; the
		 * frames after the first one use the continuation opcode, and the one with bFin = true ends it.
		 */
		template <typename T, typename TDelegate>
		static bool SendData(T* Self, EOpcode MessageOpcode, const uint8* Data, size_t Size, const FDataframe& Frame, const TDelegate& Callback) {
			const EOpcode Opcode = Self->State.bFragmenting ? EOpcode::NONE /* continuation (0x0) */ : MessageOpcode;
			if (!SendFrame(Self, Opcode, Data, Size, Frame, Callback)) {
				return false;
			}
			Self->State.bFragmenting = !Frame.bFin;
			return true;
		}

		template <typename T, typename TDelegate>
		static bool Write(T* Self, const FString& Message, const FDataframe& Frame, const TDelegate& Callback) {
			// Empty messages are valid WebSocket messages.
			const FBytes Utf8 = ToUtf8(Message);
			return SendData(Self, EOpcode::TEXT_FRAME, Utf8.data(), Utf8.size(), Frame, Callback);
		}

		template <typename T, typename TDelegate>
		static bool WriteBuffer(T* Self, const TArray<uint8>& Buffer, const FDataframe& Frame, const TDelegate& Callback) {
			return SendData(Self, EOpcode::BINARY_FRAME, Buffer.GetData(), static_cast<size_t>(Buffer.Num()), Frame, Callback);
		}

		template <typename T, typename TDelegate>
		static bool Control(T* Self, EOpcode Opcode, const TDelegate& Callback, const TArray<uint8>& Payload = TArray<uint8>()) {
			if (Payload.Num() > 125) {
				return false; // control frames carry at most 125 bytes
			}
			if (Opcode == EOpcode::PING) {
				Self->State.LastPingSeconds = FPlatformTime::Seconds();
			}
			return SendFrame(Self, Opcode, Payload.GetData(), static_cast<size_t>(Payload.Num()), FDataframe(), Callback);
		}

		static uint16 SendableCloseCode(int32 Code) {
			const uint16 Clamped = static_cast<uint16>(FMath::Clamp(Code, 0, 65535));
			if (IsSendableCloseCode(Clamped)) {
				return Clamped;
			}
			// 1005 / 1006 / 1015 and unregistered codes must never be sent (RFC 6455 7.4.1).
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol WebSocket: close code %d cannot be sent, using 1000."), Code);
			return 1000;
		}

		template <typename TRemote>
		static void AcceptUpgrade(TRemote* Remote) {
			if (Remote->State.Session) {
				Remote->State.Session->AcceptUpgrade();
			}
		}

		template <typename TRemote>
		static void RejectUpgrade(TRemote* Remote, int32 Status, const FString& Reason) {
			if (Remote->State.Session) {
				Remote->State.Session->RejectUpgrade(Status, Reason);
			}
		}

		template <typename T>
		static void End(T* Self, int32 Code, const FString& Reason) {
			if (!Self->State.Session) {
				return;
			}
			Self->State.Session->End(SendableCloseCode(Code), Reason);
			Self->State.bClosing = true;
		}

		template <typename T>
		static void Close(T* Self, int32 Code, const FString& Reason) {
			if (!Self->State.Session) {
				return;
			}
			Self->State.Session->Close(SendableCloseCode(Code), Reason);
			// The session aborts the transport; the final OnClose(Code, Reason) is still delivered
			// (PendingCloseIds), nothing else from this connection is.
			Self->State.Net.Transport.reset();
			FFacadeAccess::AddPendingClose(Self->State.Net, Self->State.Net.SessionId);
			Self->State.Net.SessionId = 0;
			Self->State.Net.bOpen = false;
			Self->State.Session.reset();
			Self->State.bClosing = false;
		}

		template <typename T>
		static void Release(T* Self) {
			if (Self->State.Session) {
				// Object destroyed: close politely without anybody listening.
				Self->State.Session->End(1001, TEXT("Going Away"));
			}
			Self->State.Session.reset();
			if (Self->State.Net.Transport) {
				// Do not Close() the transport here: the session needs it to finish the closing handshake
				// (bounded by its own timeout). Dropping our reference is enough.
				Self->State.Net.Transport.reset();
			}
			Self->State.Net.SessionId = 0;
			Self->State.Net.PendingCloseIds.Reset();
		}
	};
}
