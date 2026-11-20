/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/facade.h"
#include "net/http.h"
#include "http/httpclient.h"
#include "http/httpremote.h"
#include "Containers/Ticker.h"

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
		explicit FHttpServerSession(std::function<void(FHttpRequest&&)> InOnRequest, const FHttpLimits& Limits = FHttpLimits())
			: Parser(FHttpParser::EMode::Request, Limits), OnRequest(MoveTemp(InOnRequest)) {
			// "Expect: 100-continue": the client waits for this before sending the body (RFC 9110 10.1.1).
			Parser.OnRequestHeaders = [this](const FHttpRequest& Request) {
				const FString* Expect = FindHeader(Request.Headers, TEXT("expect"));
				if (Expect && Expect->Equals(TEXT("100-continue"), ESearchCase::IgnoreCase) && Request.Version == TEXT("1.1")) {
					if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
						static const char Continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
						Strong->Send(FBytes(Continue, Continue + sizeof(Continue) - 1));
					}
				}
			};
		}

		std::weak_ptr<FTransport> Transport;

		/** Pipelined bytes buffered while a response is pending, before the connection is dropped. */
		static constexpr size_t MaxPipelinedBytes = 1024 * 1024;

		/** Strand. */
		void OnData(const uint8* Data, size_t Size) {
			if (bFailed) {
				return; // the error response is on its way and the connection closes: ignore the rest
			}
			Parser.Append(Data, Size);
			if (bAwaiting && Parser.BufferedBytes() > MaxPipelinedBytes) {
				// A client flooding pipelined requests faster than they are answered: memory stays bounded.
				bFailed = true;
				if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
					Strong->Close();
				}
				return;
			}
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
				if (!bEstablished) {
					// Headers (and body) of the first request arrived in time: stop the handshake timeout.
					bEstablished = true;
					if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
						Strong->MarkEstablished();
					}
				}
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
		bool bEstablished = false;
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
				if (Bytes.empty()) {
					FailFront(Self, FErrorCode(MakeError(EError::InvalidArgument)));
					continue;
				}
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
						// 1xx (100 Continue, 103 Early Hints...) are interim: the final response follows on the
						// same request. 101 Switching Protocols is final (the connection changes protocol).
						if (Response.Status_Code >= 100 && Response.Status_Code < 200 && Response.Status_Code != 101) {
							continue;
						}
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
				// Nothing of a response arrived: the server closed an idle keep-alive connection under us.
				const bool bNothingReceived = !Parser->HasPartialMessage();
				// A response delimited by the end of the connection is complete now.
				if (Parser->OnEof() == FHttpParser::EResult::Complete) {
					FHttpResponse Response = MoveTemp(Parser->Response);
					RunOnGameThread(Weak, [Id, Response = MoveTemp(Response), Tls, bTls](T* Owner) {
						OnResponse(Owner, Id, Response, Tls, bTls);
					});
				}
				RunOnGameThread(Weak, [Id, Tls, bTls, bNothingReceived](T* Owner) {
					OnConnectionClosed(Owner, Id, Tls, bTls, bNothingReceived);
				});
			};

			FTransportOptions Options = ClientTransportOptions(Self->bind_options, false);
			Options.IdleTimeout = std::chrono::seconds(Self->IdleTimeoutSeconds);
			std::shared_ptr<FTransport> Transport = bTls ? MakeTlsClient(Tls, MoveTemp(Events), Options)
			                                             : MakeTcpClient(MoveTemp(Events), Options);
			if (!Transport) {
				FailAll(Self, FErrorCode(MakeError(EError::NotRunning)));
				return;
			}
			FFacadeAccess::BeginSession(S.Net, Transport, Id);
			S.Parser = Parser;
			S.ResponsesOnConnection = 0;
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
			++S.ResponsesOnConnection;

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

		static bool IsIdempotent(ERequestMethod Method) {
			return Method == ERequestMethod::GET || Method == ERequestMethod::HEAD || Method == ERequestMethod::OPTIONS ||
			       Method == ERequestMethod::PUT || Method == ERequestMethod::DEL;
		}

		template <typename T>
		static void OnConnectionClosed(T* Self, uint64 Id, const std::shared_ptr<const FTlsConfig>& Tls, bool bTls, bool bNothingReceived = false) {
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
				const bool bReused = S.ResponsesOnConnection > 0;
				if (bReused && bNothingReceived && S.Queue.Num() > 0 && !S.Queue[0].bRetried && IsIdempotent(S.Queue[0].Request.Method)) {
					// Stale keep-alive connection (closed by the server while idle): send it again once on a new
					// connection. Only idempotent methods: a POST might have been processed already.
					S.Queue[0].bRetried = true;
					S.bInFlight = false;
				} else {
					FailFront(Self, Error);
				}
			}
			Pump(Self, Tls, bTls);
		}

		/**
		 * The target changed: the current connection must not be reused, and requests already sent or queued
		 * for the previous host fail (Cancelled) instead of waiting forever for a response that cannot come.
		 */
		template <typename T>
		static void ChangeHost(T* Self) {
			Detach(Self->State);
			FailAll(Self, FErrorCode(MakeError(EError::Cancelled)));
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

			FHttpLimits Limits;
			Limits.MaxHeaderBytes = static_cast<size_t>(FMath::Max(Server->MaxHeaderBytes, 1024));
			Limits.MaxBodyBytes = static_cast<size_t>(FMath::Max<int64>(Server->MaxBodyBytes, 0));
			std::shared_ptr<FHttpServerSession> Session = std::make_shared<FHttpServerSession>(
				[Weak, Id](FHttpRequest&& Request) {
					RunOnGameThread(Weak, [Id, Request = MoveTemp(Request)](TRemote* Owner) {
						if (Owner->Net.SessionId == Id && Owner->OnRequestNative) {
							Owner->OnRequestNative(Request);
						}
					});
				}, Limits);

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

			FTransportOptions Options = AcceptedTransportOptions(Server->Server.Options, false, false);
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

		// ---- routing ----

		template <typename TDelegate>
		static void AddRoute(TArray<TPair<FString, TDelegate>>& Routes, const FString& Path, const TDelegate& Callback) {
			for (TPair<FString, TDelegate>& Route : Routes) {
				if (Route.Key.Equals(Path, ESearchCase::CaseSensitive)) {
					Route.Value = Callback;
					return;
				}
			}
			Routes.Emplace(Path, Callback);
		}

		/**
		 * Case-sensitive match of a route pattern against a path. Pattern segments: literal, ":name"
		 * (one segment) or a final "*" (the rest of the path, stored as "*").
		 */
		static bool MatchRoute(const FString& Pattern, const FString& Path, TMap<FString, FString>& OutParams) {
			if (Pattern.Equals(Path, ESearchCase::CaseSensitive)) {
				return true;
			}
			TArray<FString> P, S;
			Pattern.ParseIntoArray(P, TEXT("/"), true);
			Path.ParseIntoArray(S, TEXT("/"), true);
			TMap<FString, FString> Params;
			for (int32 Index = 0; Index < P.Num(); ++Index) {
				if (P[Index] == TEXT("*") && Index == P.Num() - 1) {
					TArray<FString> Rest(S.GetData() + FMath::Min(Index, S.Num()), FMath::Max(S.Num() - Index, 0));
					Params.Add(TEXT("*"), FString::Join(Rest, TEXT("/")));
					OutParams = MoveTemp(Params);
					return true;
				}
				if (Index >= S.Num()) {
					return false;
				}
				if (P[Index].StartsWith(TEXT(":")) && P[Index].Len() > 1) {
					Params.Add(P[Index].Mid(1), S[Index]);
				} else if (!P[Index].Equals(S[Index], ESearchCase::CaseSensitive)) {
					return false;
				}
			}
			if (P.Num() != S.Num()) {
				return false;
			}
			OutParams = MoveTemp(Params);
			return true;
		}

		/** Exact routes win over patterns; among patterns, the first registered wins. */
		template <typename TDelegate>
		static const TDelegate* FindRoute(const TArray<TPair<FString, TDelegate>>& Routes, const FString& Path, TMap<FString, FString>& OutParams) {
			for (const TPair<FString, TDelegate>& Route : Routes) {
				if (Route.Key.Equals(Path, ESearchCase::CaseSensitive)) {
					OutParams.Reset();
					return &Route.Value;
				}
			}
			for (const TPair<FString, TDelegate>& Route : Routes) {
				if (MatchRoute(Route.Key, Path, OutParams)) {
					return &Route.Value;
				}
			}
			return nullptr;
		}

		template <typename TDelegate, typename TRemote>
		static void RouteRequest(TRemote* Remote, const FHttpRequest& Request, float ResponseTimeoutSeconds,
		                         const TArray<TPair<FString, TDelegate>>& All, const TArray<TPair<FString, TDelegate>>& Get,
		                         const TArray<TPair<FString, TDelegate>>& Post, const TArray<TPair<FString, TDelegate>>& Put,
		                         const TArray<TPair<FString, TDelegate>>& Del, const TArray<TPair<FString, TDelegate>>& Head,
		                         const TArray<TPair<FString, TDelegate>>& Options, const TArray<TPair<FString, TDelegate>>& Patch) {
			struct FMethodRoutes {
				ERequestMethod Method;
				const TCHAR* Name;
				const TArray<TPair<FString, TDelegate>>* Routes;
			};
			const FMethodRoutes Table[] = {
				{ERequestMethod::DEL, TEXT("DELETE"), &Del}, {ERequestMethod::GET, TEXT("GET"), &Get},
				{ERequestMethod::HEAD, TEXT("HEAD"), &Head}, {ERequestMethod::OPTIONS, TEXT("OPTIONS"), &Options},
				{ERequestMethod::PATCH, TEXT("PATCH"), &Patch}, {ERequestMethod::POST, TEXT("POST"), &Post},
				{ERequestMethod::PUT, TEXT("PUT"), &Put},
			};
			auto Answer = [Remote](int32 Status, const TCHAR* Message, const FString& Body) {
				Remote->Headers.Status_Code = Status;
				Remote->Headers.Status_Message = Message;
				Remote->Headers.Body = Body;
				Remote->Write(FDelegateHttpRemoteMessageSent());
			};
			if (Request.Method == ERequestMethod::UNKNOWN) {
				Remote->Headers.Headers.Add(TEXT("Allow"), TEXT("DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT"));
				Answer(405, TEXT("Method Not Allowed"), TEXT("Method not supported."));
				return;
			}

			// Copy the delegates: a handler may register routes or close the server while we iterate.
			TArray<TPair<TDelegate, TMap<FString, FString>>> Handlers;
			TMap<FString, FString> Params;
			if (const TDelegate* Any = FindRoute(All, Request.Path, Params)) {
				Handlers.Emplace(*Any, Params);
			}
			const TArray<TPair<FString, TDelegate>>* MethodRoutes = nullptr;
			for (const FMethodRoutes& Entry : Table) {
				if (Entry.Method == Request.Method) {
					MethodRoutes = Entry.Routes;
				}
			}
			const TDelegate* Specific = MethodRoutes ? FindRoute(*MethodRoutes, Request.Path, Params) : nullptr;
			if (!Specific && Request.Method == ERequestMethod::HEAD) {
				// HEAD is GET without the body (the body is dropped when the response is written).
				Specific = FindRoute(Get, Request.Path, Params);
			}
			if (Specific) {
				Handlers.Emplace(*Specific, Params);
			}

			bool bHandled = false;
			for (const TPair<TDelegate, TMap<FString, FString>>& Handler : Handlers) {
				if (Handler.Key.IsBound()) {
					bHandled = true;
					FHttpRequest Routed = Request;
					Routed.RouteParams = Handler.Value;
					Handler.Key.ExecuteIfBound(Routed, Remote);
				}
			}
			if (!IsValid(Remote)) {
				return;
			}
			if (bHandled) {
				ArmResponseTimeout(Remote, ResponseTimeoutSeconds);
				return;
			}

			// The path exists for other methods: 405 with the allowed ones, otherwise 404.
			TArray<FString> Allowed;
			for (const FMethodRoutes& Entry : Table) {
				TMap<FString, FString> Ignored;
				if (FindRoute(*Entry.Routes, Request.Path, Ignored)) {
					Allowed.AddUnique(Entry.Name);
					if (Entry.Method == ERequestMethod::GET) {
						Allowed.AddUnique(TEXT("HEAD"));
					}
				}
			}
			if (Allowed.Num() > 0) {
				Allowed.Sort();
				Remote->Headers.Headers.Add(TEXT("Allow"), FString::Join(Allowed, TEXT(", ")));
				Answer(405, TEXT("Method Not Allowed"), TEXT("Method Not Allowed"));
				return;
			}
			Answer(404, TEXT("Not Found"), TEXT("Not Found"));
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
			++Remote->ResponseSerial;
		}

		/**
		 * A handler that never calls Write would block the connection forever (responses are ordered): after
		 * Seconds the request is answered with 503 automatically. 0 disables it.
		 */
		template <typename TRemote>
		static void ArmResponseTimeout(TRemote* Remote, float Seconds) {
			if (Seconds <= 0.0f || !Remote->bResponsePending) {
				return;
			}
			TWeakObjectPtr<TRemote> Weak(Remote);
			const uint32 Serial = Remote->ResponseSerial;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, Serial](float) {
				TRemote* Owner = Weak.Get();
				if (Owner && Owner->bResponsePending && Owner->ResponseSerial == Serial) {
					UE_LOG(LogAsio, Warning, TEXT("InternetProtocol HTTP: request handler did not respond in time, sending 503."));
					Owner->Headers.Status_Code = 503;
					Owner->Headers.Status_Message = TEXT("Service Unavailable");
					Owner->Headers.Headers.Add(TEXT("Content-Type"), TEXT("text/plain"));
					Owner->Headers.Body = TEXT("The request was not answered in time.");
					Owner->Write(FDelegateHttpRemoteMessageSent());
				}
				return false;
			}), Seconds);
		}

		template <typename TRemote, typename TDelegate>
		static bool WriteResponse(TRemote* Remote, const TDelegate& Callback) {
			if (!Remote->Net.Transport || !Remote->bResponsePending) {
				return false;
			}
			Remote->bResponsePending = false;
			// The handler may ask to close the connection with "Connection: close".
			const FString* HandlerConnection = FindHeader(Remote->Headers.Headers, TEXT("Connection"));
			const bool bClose = Remote->bWillClose || (HandlerConnection && HeaderHasToken(*HandlerConnection, TEXT("close")));
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
