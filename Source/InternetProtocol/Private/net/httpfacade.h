/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/facade.h"
#include "net/http.h"
#include "http/httpclient.h"

/*
 * HTTP facades.
 *  Client: requests are queued on the Game Thread and sent one at a time on a keep-alive connection. The
 *          response parser runs on the transport strand. Every callback fires exactly once: with the
 *          response, or with an error (connection failure, parse error, connection closed, Close()).
 *  Server: one FHttpServerSession per connection parses requests on the strand. Parsing pauses after each
 *          request until the response has been written (correct ordering for pipelined requests). Requests
 *          are routed on the Game Thread; a request nobody answers receives 404 automatically.
 */
namespace IPNet {

	class FHttpServerSession : public std::enable_shared_from_this<FHttpServerSession> {
	public:
		explicit FHttpServerSession(std::function<void(FHttpRequest&&)> InOnRequest)
			: Parser(FHttpParser::EMode::Request), OnRequest(MoveTemp(InOnRequest)) {}

		std::weak_ptr<FTransport> Transport;

		/** Strand. */
		void OnData(const uint8* Data, size_t Size) {
			Parser.Append(Data, Size);
			ParseLoop();
		}

		/** Strand: the response of the current request has been written. */
		void Resume() {
			bAwaiting = false;
			ParseLoop();
		}

		void ReleaseCallbacks() { OnRequest = nullptr; }

	private:
		void ParseLoop() {
			if (bAwaiting || bFailed) {
				return;
			}
			const FHttpParser::EResult Result = Parser.Parse();
			if (Result == FHttpParser::EResult::Complete) {
				bAwaiting = true;
				FHttpRequest Request = MoveTemp(Parser.Request);
				Parser.Next();
				if (OnRequest) {
					OnRequest(MoveTemp(Request));
				}
			} else if (Result == FHttpParser::EResult::Error) {
				bFailed = true;
				if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
					FHttpResponse Response;
					Response.Status_Code = Parser.ErrorStatus;
					Response.Headers.Add(TEXT("Content-Type"), TEXT("text/plain"));
					Response.Body = Parser.ErrorMessage;
					Strong->Send(SerializeResponse(Response, false, true));
					Strong->Close();
				}
			}
		}

		FHttpParser Parser;
		std::function<void(FHttpRequest&&)> OnRequest;
		bool bAwaiting = false;
		bool bFailed = false;
	};

	struct FHttpAccess {
		// ------------------------------------------------------------------------------------------------
		// Client (UHttpClient / UHttpClientSsl). Required members: FIpHttpClientState State;
		// FClientBindOptions bind_options; uint8 IdleTimeoutSeconds.

		static FString HostHeader(const FClientBindOptions& Options, bool bTls) {
			FString Host = Options.Address;
			if (Host.Contains(TEXT(":")) && !Host.StartsWith(TEXT("["))) {
				Host = TEXT("[") + Host + TEXT("]"); // IPv6 literal
			}
			const int32 DefaultPort = bTls ? 443 : 80;
			if (!Options.Port.IsEmpty() && FCString::Atoi(*Options.Port) != DefaultPort) {
				Host += TEXT(":") + Options.Port;
			}
			return Host;
		}

		template <typename T>
		static void Request(T* Self, const FHttpRequest& Request, const FDelegateHttpClientResponse& Callback,
		                    const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			Self->State.Queue.Add(FIpHttpPendingRequest{Request, Callback});
			Pump(Self, Tls, bTls);
		}

		template <typename T>
		static void Pump(T* Self, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			FIpHttpClientState& S = Self->State;
			while (!S.bInFlight && S.Queue.Num() > 0) {
				if (!S.Net.Transport) {
					Connect(Self, Tls, bTls);
					return;
				}
				if (!S.Net.bOpen) {
					return; // OnConnected pumps again
				}
				const FIpHttpPendingRequest& Front = S.Queue[0];
				const bool bHead = Front.Request.Method == ERequestMethod::HEAD;
				FBytes Bytes = SerializeRequest(Front.Request, HostHeader(Self->bind_options, bTls));
				std::shared_ptr<FHttpParser> Parser = S.Parser;
				S.Net.Transport->Post([Parser, bHead]() { Parser->SetNoBody(bHead); });
				if (S.Net.Transport->Send(MoveTemp(Bytes))) {
					S.bInFlight = true;
					return;
				}
				FailFront(Self, FErrorCode(MakeError(EError::QueueFull)));
			}
		}

		template <typename T>
		static void Connect(T* Self, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			FIpHttpClientState& S = Self->State;
			const uint64 Id = NextSessionId();
			std::shared_ptr<FHttpParser> Parser = std::make_shared<FHttpParser>(FHttpParser::EMode::Response);
			TWeakObjectPtr<T> Weak(Self);

			FTransportEvents Events;
			Events.OnConnected = [Weak, Id, Tls, bTls](const asio::ip::tcp::endpoint& Local, const asio::ip::tcp::endpoint& Remote) {
				RunOnGameThread(Weak, [Id, Local, Remote, Tls, bTls](T* Owner) {
					if (Owner->State.Net.SessionId != Id) {
						return;
					}
					Owner->State.Net.bOpen = true;
					Owner->State.Net.Local = FTcpEndpoint(Local);
					Owner->State.Net.Remote = FTcpEndpoint(Remote);
					Pump(Owner, Tls, bTls);
				});
			};
			Events.OnData = [Weak, Id, Parser, Tls, bTls](const uint8* Data, size_t Size) {
				Parser->Append(Data, Size);
				for (;;) {
					const FHttpParser::EResult Result = Parser->Parse();
					if (Result == FHttpParser::EResult::Complete) {
						FHttpResponse Response = MoveTemp(Parser->Response);
						Parser->Next();
						RunOnGameThread(Weak, [Id, Response = MoveTemp(Response), Tls, bTls](T* Owner) {
							OnResponse(Owner, Id, Response, Tls, bTls);
						});
						continue;
					}
					if (Result == FHttpParser::EResult::Error) {
						RunOnGameThread(Weak, [Id, Tls, bTls](T* Owner) {
							if (Owner->State.Net.SessionId == Id) {
								FailFront(Owner, FErrorCode(MakeError(EError::HttpParseError)));
								Detach(Owner->State);
								Pump(Owner, Tls, bTls);
							}
						});
					}
					break;
				}
			};
			Events.OnError = [Weak, Id](const asio::error_code& Ec) {
				RunOnGameThread(Weak, [Id, Ec](T* Owner) {
					if (Owner->State.Net.SessionId == Id && Owner->State.ConnectionError.Value == 0) {
						Owner->State.ConnectionError = FErrorCode(Ec);
					}
				});
			};
			Events.OnClosed = [Weak, Id, Parser, Tls, bTls]() {
				// A response delimited by the end of the connection is complete now.
				if (Parser->OnEof() == FHttpParser::EResult::Complete) {
					FHttpResponse Response = MoveTemp(Parser->Response);
					RunOnGameThread(Weak, [Id, Response = MoveTemp(Response), Tls, bTls](T* Owner) {
						OnResponse(Owner, Id, Response, Tls, bTls);
					});
				}
				RunOnGameThread(Weak, [Id, Tls, bTls](T* Owner) {
					OnConnectionClosed(Owner, Id, Tls, bTls);
				});
			};

			FTransportOptions Options;
			Options.IdleTimeout = std::chrono::seconds(Self->IdleTimeoutSeconds);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsClient(Tls, MoveTemp(Events), Options)
			                                             : MakeTcpClient(MoveTemp(Events), Options);
			if (!Transport) {
				FailAll(Self, FErrorCode(MakeError(EError::NotRunning)));
				return;
			}
			FFacadeAccess::BeginSession(S.Net, Transport, Id);
			S.Parser = Parser;
			S.ConnectionError = FErrorCode();
			Transport->Connect(ToStd(Self->bind_options.Address), ToStd(Self->bind_options.Port), Self->bind_options.Protocol);
		}

		template <typename T>
		static void OnResponse(T* Self, uint64 Id, const FHttpResponse& Response, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			FIpHttpClientState& S = Self->State;
			if (S.Net.SessionId != Id || !S.bInFlight || S.Queue.Num() == 0) {
				return;
			}
			const FDelegateHttpClientResponse Callback = S.Queue[0].Callback;
			S.Queue.RemoveAt(0);
			S.bInFlight = false;

			const FString* Connection = FindHeader(Response.Headers, TEXT("connection"));
			const bool bKeepAlive = Response.Version == TEXT("1.0")
				                        ? (Connection && HeaderHasToken(*Connection, TEXT("keep-alive")))
				                        : !(Connection && HeaderHasToken(*Connection, TEXT("close")));
			if (!bKeepAlive) {
				Detach(S);
			}

			Callback.ExecuteIfBound(FErrorCode(), Response);
			// The callback may have destroyed state (e.g. Close()); Pump re-validates everything.
			Pump(Self, Tls, bTls);
		}

		template <typename T>
		static void OnConnectionClosed(T* Self, uint64 Id, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			FIpHttpClientState& S = Self->State;
			if (S.Net.SessionId != Id) {
				return;
			}
			const bool bWasOpen = S.Net.bOpen;
			const FErrorCode Error = S.ConnectionError.Value != 0 ? S.ConnectionError : FErrorCode(MakeError(EError::ConnectionClosed));
			Detach(S);
			if (!bWasOpen) {
				// The connection could not be established: fail everything instead of retrying forever.
				FailAll(Self, Error);
				return;
			}
			if (S.bInFlight) {
				FailFront(Self, Error);
			}
			Pump(Self, Tls, bTls);
		}

		/** Forgets the current connection without delivering anything else from it. */
		static void Detach(FIpHttpClientState& S) {
			if (S.Net.Transport) {
				S.Net.Transport->Close();
			}
			S.Net.Transport.reset();
			S.Net.SessionId = 0;
			S.Net.bOpen = false;
			S.Parser.reset();
		}

		template <typename T>
		static void FailFront(T* Self, const FErrorCode& Error) {
			FIpHttpClientState& S = Self->State;
			S.bInFlight = false;
			if (S.Queue.Num() == 0) {
				return;
			}
			const FDelegateHttpClientResponse Callback = S.Queue[0].Callback;
			S.Queue.RemoveAt(0);
			Callback.ExecuteIfBound(Error, FHttpResponse());
		}

		template <typename T>
		static void FailAll(T* Self, const FErrorCode& Error) {
			FIpHttpClientState& S = Self->State;
			S.bInFlight = false;
			TArray<FIpHttpPendingRequest> Pending = MoveTemp(S.Queue);
			S.Queue.Reset();
			for (const FIpHttpPendingRequest& Item : Pending) {
				Item.Callback.ExecuteIfBound(Error, FHttpResponse());
			}
		}

		template <typename T>
		static void Close(T* Self) {
			Detach(Self->State);
			FailAll(Self, FErrorCode(MakeError(EError::ConnectionClosed)));
		}

		static void Release(FIpHttpClientState& S) {
			Detach(S);
			S.Queue.Reset();
			S.bInFlight = false;
		}

		// ------------------------------------------------------------------------------------------------
		// Server side remote (UHttpRemote / UHttpRemoteSsl). Required members: FIpStreamState Net;
		// std::shared_ptr<FHttpServerSession> Session; FHttpResponse Headers; bool bResponsePending,
		// bWillClose, bHeadRequest; TFunction<void(const FHttpRequest&)> OnRequestNative;
		// TFunction<void()> OnClosedNative; OnClose(); OnError(Error).

		template <typename TServer, typename TRemote>
		static TRemote* AdoptRemote(TServer* Server, TSet<TRemote*>& Clients, asio::ip::tcp::socket&& Socket,
		                            const std::shared_ptr<const FTlsConfig>& Tls, bool bTls) {
			TRemote* Remote = NewObject<TRemote>(Server);
			const uint64 Id = NextSessionId();
			TWeakObjectPtr<TRemote> Weak(Remote);

			std::shared_ptr<FHttpServerSession> Session = std::make_shared<FHttpServerSession>(
				[Weak, Id](FHttpRequest&& Request) {
					RunOnGameThread(Weak, [Id, Request = MoveTemp(Request)](TRemote* Owner) {
						if (Owner->Net.SessionId == Id && Owner->OnRequestNative) {
							Owner->OnRequestNative(Request);
						}
					});
				});

			FTransportEvents Events;
			Events.OnConnected = [Weak, Id](const asio::ip::tcp::endpoint& Local, const asio::ip::tcp::endpoint& RemoteEp) {
				RunOnGameThread(Weak, [Id, Local, RemoteEp](TRemote* Owner) {
					if (Owner->Net.SessionId == Id) {
						Owner->Net.bOpen = true;
						Owner->Net.Local = FTcpEndpoint(Local);
						Owner->Net.Remote = FTcpEndpoint(RemoteEp);
					}
				});
			};
			Events.OnData = [Session](const uint8* Data, size_t Size) { Session->OnData(Data, Size); };
			Events.OnError = FFacadeAccess::MakeErrorEvent<TRemote>(Weak, Id);
			Events.OnClosed = [Weak, Id, Session]() {
				Session->ReleaseCallbacks();
				RunOnGameThread(Weak, [Id](TRemote* Owner) {
					if (FFacadeAccess::StreamClosed(Owner->Net, Id)) {
						Owner->Session.reset();
						Owner->bResponsePending = false;
						Owner->OnClose.Broadcast();
						if (Owner->OnClosedNative) {
							Owner->OnClosedNative();
						}
					}
				});
			};

			FTransportOptions Options;
			Options.IdleTimeout = std::chrono::seconds(Server->IdleTimeoutSeconds);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsAccepted(MoveTemp(Socket), Tls, MoveTemp(Events), Options)
			                                             : MakeTcpAccepted(MoveTemp(Socket), MoveTemp(Events), Options);
			if (!Transport) {
				return nullptr;
			}
			Session->Transport = Transport;
			FFacadeAccess::BeginSession(Remote->Net, Transport, Id);
			Remote->Session = Session;
			FFacadeAccess::BindRemoteToServer(Server, Clients, Remote);
			return Remote;
		}

		template <typename TRemote>
		static void SetRequestHandler(TRemote* Remote, TFunction<void(const FHttpRequest&)> Handler) {
			Remote->OnRequestNative = MoveTemp(Handler);
		}

		template <typename TRemote>
		static void PrepareResponse(TRemote* Remote, const FHttpRequest& Request) {
			Remote->Headers = FHttpResponse();
			Remote->Headers.Status_Code = 200;
			Remote->Headers.Status_Message = TEXT("OK");
			Remote->Headers.Version = Request.Version == TEXT("1.0") ? TEXT("1.0") : TEXT("1.1");
			Remote->Headers.Headers.Add(TEXT("Content-Type"), TEXT("text/plain"));
			Remote->Headers.Headers.Add(TEXT("X-Powered-By"), TEXT("ASIO"));

			const FString* Connection = FindHeader(Request.Headers, TEXT("connection"));
			Remote->bWillClose = Request.Version == TEXT("1.0")
				                     ? !(Connection && HeaderHasToken(*Connection, TEXT("keep-alive")))
				                     : (Connection && HeaderHasToken(*Connection, TEXT("close")));
			if (!Remote->bWillClose && Request.Version == TEXT("1.0")) {
				Remote->Headers.Headers.Add(TEXT("Connection"), TEXT("keep-alive"));
			}
			Remote->bHeadRequest = Request.Method == ERequestMethod::HEAD;
			Remote->bResponsePending = true;
		}

		template <typename TRemote, typename TDelegate>
		static bool WriteResponse(TRemote* Remote, const TDelegate& Callback) {
			if (!Remote->Net.Transport || !Remote->bResponsePending) {
				return false;
			}
			Remote->bResponsePending = false;
			const bool bClose = Remote->bWillClose;
			FBytes Bytes = SerializeResponse(Remote->Headers, Remote->bHeadRequest, bClose);

			FSendCallback UserCallback = MakeSendCallback(Remote, Callback);
			std::weak_ptr<FHttpServerSession> WeakSession = Remote->Session;
			std::weak_ptr<FTransport> WeakTransport = Remote->Net.Transport;
			return Remote->Net.Transport->Send(MoveTemp(Bytes),
				[UserCallback, WeakSession, WeakTransport, bClose](const asio::error_code& Ec, size_t Bytes) {
					if (UserCallback) {
						UserCallback(Ec, Bytes);
					}
					if (Ec) {
						return;
					}
					if (bClose) {
						if (std::shared_ptr<FTransport> Transport = WeakTransport.lock()) {
							Transport->Close();
						}
					} else if (std::shared_ptr<FHttpServerSession> Session = WeakSession.lock()) {
						Session->Resume();
					}
				});
		}

		template <typename TRemote>
		static void ReleaseRemote(TRemote* Remote) {
			FFacadeAccess::ReleaseStream(Remote->Net);
			Remote->Session.reset();
			Remote->OnRequestNative = nullptr;
			Remote->OnClosedNative = nullptr;
		}

		template <typename TRemote>
		static void CloseRemote(TRemote* Remote) {
			FFacadeAccess::CloseStream(Remote->Net);
			Remote->bResponsePending = false;
		}

		template <typename TRemote>
		static void StartRemote(TRemote* Remote) {
			FFacadeAccess::StartRemote(Remote);
		}
	};
}
