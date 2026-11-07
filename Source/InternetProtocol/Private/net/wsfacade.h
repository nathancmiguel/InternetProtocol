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

		template <typename T>
		static FWsSession::FCallbacks MakeCommonCallbacks(const TWeakObjectPtr<T>& Weak, uint64 Id) {
			FWsSession::FCallbacks Callbacks;
			Callbacks.OnMessage = [Weak, Id](FBytes&& Payload, bool bBinary) {
				TArray<uint8> Buffer(Payload.data(), static_cast<int32>(Payload.size()));
				RunOnGameThread(Weak, [Id, bBinary, Buffer = MoveTemp(Buffer)](T* Self) {
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
					if (Self->State.Net.SessionId == Id || Self->State.Net.PendingCloseId == Id) {
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
					if (FFacadeAccess::StreamClosed(Self->State.Net, Id)) {
						Self->State.Session.reset();
						Self->State.bClosing = false;
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
			for (const TCHAR* Name : {TEXT("Upgrade"), TEXT("Connection"), TEXT("Sec-WebSocket-Key"), TEXT("Sec-WebSocket-Version")}) {
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

			FWsSession::FCallbacks Callbacks = MakeCommonCallbacks(Weak, Id);
			Callbacks.OnClientHandshake = [Weak, Id](FHttpResponse&& Response) {
				RunOnGameThread(Weak, [Id, Response = MoveTemp(Response)](T* Owner) {
					if (Owner->State.Net.SessionId == Id) {
						Owner->State.Net.bOpen = true;
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
			std::shared_ptr<FWsSession> Session = std::make_shared<FWsSession>(true, MoveTemp(Callbacks), MoveTemp(HandshakeBytes), Key);

			FTransportEvents Events = MakeTransportEvents(Weak, Id, Session);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsClient(Tls, MoveTemp(Events)) : MakeTcpClient(MoveTemp(Events));
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

			FWsSession::FCallbacks Callbacks = MakeCommonCallbacks(Weak, Id);
			Callbacks.OnServerHandshake = [Weak, Id](FHttpRequest&& Request) {
				RunOnGameThread(Weak, [Id, Request = MoveTemp(Request)](TRemote* Owner) {
					if (Owner->State.Net.SessionId == Id) {
						Owner->State.Net.bOpen = true;
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
			std::shared_ptr<FWsSession> Session = std::make_shared<FWsSession>(false, MoveTemp(Callbacks));

			FTransportEvents Events = MakeTransportEvents(Weak, Id, Session);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events))
			                                             : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events));
			if (!Transport) {
				return nullptr;
			}
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
			FBytes Bytes = EncodeWsFrame(Opcode, Data, Size, Frame.bFin, Frame.bRsv1, Frame.bRsv2, Frame.bRsv3, bMask);
			Self->State.Session->SendFrame(MoveTemp(Bytes), MakeSendCallback(Self, Callback));
			return true;
		}

		template <typename T, typename TDelegate>
		static bool Write(T* Self, const FString& Message, const FDataframe& Frame, const TDelegate& Callback) {
			if (Message.IsEmpty()) {
				return false;
			}
			const FBytes Utf8 = ToUtf8(Message);
			return SendFrame(Self, EOpcode::TEXT_FRAME, Utf8.data(), Utf8.size(), Frame, Callback);
		}

		template <typename T, typename TDelegate>
		static bool WriteBuffer(T* Self, const TArray<uint8>& Buffer, const FDataframe& Frame, const TDelegate& Callback) {
			if (Buffer.Num() == 0) {
				return false;
			}
			return SendFrame(Self, EOpcode::BINARY_FRAME, Buffer.GetData(), static_cast<size_t>(Buffer.Num()), Frame, Callback);
		}

		template <typename T, typename TDelegate>
		static bool Control(T* Self, EOpcode Opcode, const TDelegate& Callback) {
			return SendFrame(Self, Opcode, nullptr, 0, FDataframe(), Callback);
		}

		template <typename T>
		static void End(T* Self, int32 Code, const FString& Reason) {
			if (!Self->State.Session) {
				return;
			}
			Self->State.Session->End(static_cast<uint16>(FMath::Clamp(Code, 0, 65535)), Reason);
			Self->State.bClosing = true;
		}

		template <typename T>
		static void Close(T* Self, int32 Code, const FString& Reason) {
			if (!Self->State.Session) {
				return;
			}
			Self->State.Session->Close(static_cast<uint16>(FMath::Clamp(Code, 0, 65535)), Reason);
			// The session aborts the transport; the final OnClose(Code, Reason) is still delivered
			// (PendingCloseId), nothing else from this connection is.
			Self->State.Net.Transport.reset();
			Self->State.Net.PendingCloseId = Self->State.Net.SessionId;
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
			Self->State.Net.PendingCloseId = 0;
		}
	};
}
