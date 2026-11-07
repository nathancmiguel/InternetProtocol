/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/wssession.h"

namespace IPNet {
	namespace {
		constexpr std::chrono::milliseconds CloseHandshakeTimeout{5000};
	}

	FWsSession::FWsSession(bool bInClient, FCallbacks InCallbacks, FBytes InHandshakeBytes, FString InKey)
		: bClient(bInClient),
		  Callbacks(MoveTemp(InCallbacks)),
		  HandshakeBytes(MoveTemp(InHandshakeBytes)),
		  Key(MoveTemp(InKey)),
		  HandshakeParser(bInClient ? FHttpParser::EMode::Response : FHttpParser::EMode::Request),
		  Frames(/*bExpectMasked*/ !bInClient) {
		HandshakeParser.SetHeadersOnly(true);
	}

	void FWsSession::RunOnStrand(std::function<void()> Function) {
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			Strong->Post(MoveTemp(Function));
		}
	}

	void FWsSession::Send(FBytes&& Bytes, FSendCallback Callback) {
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			if (!Strong->Send(MoveTemp(Bytes), Callback) && Callback) {
				Callback(asio::error::operation_aborted, 0);
			}
		} else if (Callback) {
			Callback(asio::error::operation_aborted, 0);
		}
	}

	void FWsSession::CloseTransport() {
		if (std::shared_ptr<FTransport> Strong = Transport.lock()) {
			Strong->Close();
		}
	}

	void FWsSession::OnTransportConnected() {
		if (bClient && State == EState::Handshake) {
			Send(MoveTemp(HandshakeBytes));
			HandshakeBytes = FBytes();
		}
	}

	void FWsSession::OnData(const uint8* Data, size_t Size) {
		if (State == EState::Closed) {
			return;
		}
		if (State == EState::Handshake) {
			HandshakeParser.Append(Data, Size);
			HandleHandshake();
			return;
		}
		Frames.Append(Data, Size);
		ProcessFrames();
	}

	void FWsSession::HandleHandshake() {
		const FHttpParser::EResult Result = HandshakeParser.Parse();
		if (Result == FHttpParser::EResult::NeedMore) {
			return;
		}

		FString Reason;
		if (bClient) {
			FHttpResponse Response = MoveTemp(HandshakeParser.Response);
			if (Result == FHttpParser::EResult::Complete && ValidateWsUpgradeResponse(Response, Key, Reason)) {
				State = EState::Open;
				const FBytes Remaining = HandshakeParser.TakeRemaining();
				if (Callbacks.OnClientHandshake) {
					Callbacks.OnClientHandshake(MoveTemp(Response));
				}
				Frames.Append(Remaining.data(), Remaining.size());
				ProcessFrames();
				return;
			}
			UE_LOG(LogAsio, Warning, TEXT("InternetProtocol WebSocket: handshake rejected: %s"),
			       Result == FHttpParser::EResult::Error ? *HandshakeParser.ErrorMessage : *Reason);
			State = EState::Closed;
			CloseCode = 1002;
			CloseReason = TEXT("Protocol error");
			if (Response.Body.IsEmpty()) {
				Response.Body = Reason;
			}
			if (Callbacks.OnClientUnexpected) {
				Callbacks.OnClientUnexpected(MoveTemp(Response));
			}
			CloseTransport();
			return;
		}

		FHttpRequest Request = MoveTemp(HandshakeParser.Request);
		if (Result == FHttpParser::EResult::Complete && ValidateWsUpgradeRequest(Request, Reason)) {
			const FString* RequestKey = FindHeader(Request.Headers, TEXT("Sec-WebSocket-Key"));
			FHttpResponse Upgrade;
			Upgrade.Status_Code = 101;
			Upgrade.Status_Message = TEXT("Switching Protocols");
			Upgrade.Headers.Add(TEXT("Upgrade"), TEXT("websocket"));
			Upgrade.Headers.Add(TEXT("Sec-WebSocket-Accept"), ComputeWebSocketAccept(RequestKey ? *RequestKey : FString()));
			FHttpResponse UpgradeWithConnection = Upgrade;
			UpgradeWithConnection.Headers.Add(TEXT("Connection"), TEXT("Upgrade"));
			Send(SerializeResponse(UpgradeWithConnection, true, false));

			State = EState::Open;
			const FBytes Remaining = HandshakeParser.TakeRemaining();
			if (Callbacks.OnServerHandshake) {
				Callbacks.OnServerHandshake(MoveTemp(Request));
			}
			Frames.Append(Remaining.data(), Remaining.size());
			ProcessFrames();
			return;
		}

		const int32 Status = Result == FHttpParser::EResult::Error ? HandshakeParser.ErrorStatus : 400;
		FHttpResponse Rejection;
		Rejection.Status_Code = Status;
		Rejection.Headers.Add(TEXT("Content-Type"), TEXT("text/plain"));
		Rejection.Body = Result == FHttpParser::EResult::Error ? HandshakeParser.ErrorMessage : Reason;
		if (Status == 400 && Result != FHttpParser::EResult::Error) {
			Rejection.Headers.Add(TEXT("Sec-WebSocket-Version"), TEXT("13"));
		}
		Send(SerializeResponse(Rejection, false, true));
		State = EState::Closed;
		CloseCode = 1002;
		CloseReason = TEXT("Protocol error");
		if (Callbacks.OnServerUnexpected) {
			Callbacks.OnServerUnexpected(MoveTemp(Request));
		}
		CloseTransport();
	}

	void FWsSession::ProcessFrames() {
		FWsEvent Event;
		while (State != EState::Closed && Frames.Next(Event)) {
			switch (Event.Type) {
			case FWsEvent::EType::Message:
				if (Callbacks.OnMessage) {
					Callbacks.OnMessage(MoveTemp(Event.Payload), Event.bBinary);
				}
				break;
			case FWsEvent::EType::Ping:
				if (State == EState::Open) {
					Send(EncodeWsFrame(EOpcode::PONG, Event.Payload.data(), Event.Payload.size(), true, false, false, false, bClient));
				}
				if (Callbacks.OnPing) {
					Callbacks.OnPing();
				}
				break;
			case FWsEvent::EType::Pong:
				if (Callbacks.OnPong) {
					Callbacks.OnPong();
				}
				break;
			case FWsEvent::EType::Close:
				bCloseReceived = true;
				CloseCode = Event.Code;
				CloseReason = Event.Code == 1005 ? FString() : Event.Reason;
				if (!bCloseSent) {
					// Echo the status code (RFC 6455 section 5.5.1).
					SendClose(Event.Code == 1005 ? 1000 : Event.Code, FString());
					CloseCode = Event.Code;
					CloseReason = Event.Code == 1005 ? FString() : Event.Reason;
				}
				State = EState::Closing;
				CloseTransport();
				return;
			case FWsEvent::EType::ProtocolError:
				UE_LOG(LogAsio, Warning, TEXT("InternetProtocol WebSocket: protocol error %d (%s)"), Event.Code, *Event.Reason);
				if (!bCloseSent) {
					SendClose(Event.Code, Event.Reason);
				}
				CloseCode = Event.Code;
				CloseReason = Event.Reason;
				State = EState::Closing;
				CloseTransport();
				return;
			}
		}
	}

	void FWsSession::SendClose(uint16 Code, const FString& Reason) {
		bCloseSent = true;
		Send(EncodeWsCloseFrame(Code, Reason, bClient));
	}

	void FWsSession::SendFrame(FBytes Frame, FSendCallback Callback) {
		std::shared_ptr<FWsSession> Self = shared_from_this();
		RunOnStrand([Self, Frame = MoveTemp(Frame), Callback = MoveTemp(Callback)]() mutable {
			if (Self->State != EState::Open || Self->bCloseSent) {
				if (Callback) {
					Callback(asio::error::not_connected, 0);
				}
				return;
			}
			Self->Send(MoveTemp(Frame), MoveTemp(Callback));
		});
	}

	void FWsSession::End(uint16 Code, const FString& Reason) {
		std::shared_ptr<FWsSession> Self = shared_from_this();
		RunOnStrand([Self, Code, Reason]() {
			if (Self->State == EState::Handshake) {
				Self->State = EState::Closed;
				Self->CloseCode = Code;
				Self->CloseReason = Reason;
				Self->CloseTransport();
				return;
			}
			if (Self->State != EState::Open || Self->bCloseSent) {
				return;
			}
			Self->SendClose(Code, Reason);
			Self->CloseCode = Code;
			Self->CloseReason = Reason;
			Self->State = EState::Closing;
			// Wait for the peer's Close frame, but not forever.
			if (std::shared_ptr<FTransport> Strong = Self->Transport.lock()) {
				Strong->CloseAfter(CloseHandshakeTimeout);
			}
		});
	}

	void FWsSession::Close(uint16 Code, const FString& Reason) {
		std::shared_ptr<FWsSession> Self = shared_from_this();
		RunOnStrand([Self, Code, Reason]() {
			if (Self->State == EState::Closed && Self->bUserAbort) {
				return;
			}
			Self->bUserAbort = true;
			Self->CloseCode = Code;
			Self->CloseReason = Reason;
			Self->State = EState::Closed;
			if (std::shared_ptr<FTransport> Strong = Self->Transport.lock()) {
				Strong->abort();
			}
		});
	}

	void FWsSession::OnTransportClosed(uint16& OutCode, FString& OutReason) {
		State = EState::Closed;
		OutCode = CloseCode;
		OutReason = CloseReason;
		Callbacks = FCallbacks();
	}
}
