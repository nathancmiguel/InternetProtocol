/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/http.h"
#include "net/transport.h"
#include "net/websocket.h"
#include <functional>
#include <memory>

/*
 * WebSocket protocol state machine for one connection (client or server role), running on the strand of
 * its transport. It performs the opening handshake, decodes frames, answers pings, and implements the
 * closing handshake (RFC 6455 section 7):
 *  - End(code, reason): sends a Close frame and waits (max 5 s) for the peer's Close, then closes TCP.
 *  - receiving a Close: echoes it and closes TCP.
 *  - protocol violation: sends Close with 1002/1007/1009 and closes TCP.
 *  - Close(code, reason): immediate abort; the given code is reported.
 *  - connection lost without a Close frame: 1006.
 * Results are reported through FCallbacks (which forward to the Game Thread). The session only holds a
 * weak reference to its transport, and the transport releases the session callbacks once it is closed.
 */
namespace IPNet {

	class FWsSession : public std::enable_shared_from_this<FWsSession> {
	public:
		struct FCallbacks {
			std::function<void(FHttpResponse&&)> OnClientHandshake;
			std::function<void(FHttpRequest&&)> OnServerHandshake;
			std::function<void(FHttpResponse&&)> OnClientUnexpected;
			std::function<void(FHttpRequest&&)> OnServerUnexpected;
			std::function<void(FBytes&&, bool)> OnMessage;
			std::function<void()> OnPing;
			std::function<void()> OnPong;
		};

		/** Client: HandshakeBytes is the serialized upgrade request and Key its Sec-WebSocket-Key. */
		FWsSession(bool bInClient, FCallbacks InCallbacks, FBytes InHandshakeBytes = {}, FString InKey = FString());

		std::weak_ptr<FTransport> Transport;

		// ---- strand ----
		void OnTransportConnected();
		void OnData(const uint8* Data, size_t Size);
		/** Final close code/reason; releases the callbacks. */
		void OnTransportClosed(uint16& OutCode, FString& OutReason);

		// ---- any thread (posted to the transport strand) ----
		/** Sends an already encoded data/control frame if the connection is open. */
		void SendFrame(FBytes Frame, FSendCallback Callback);
		void End(uint16 Code, const FString& Reason);
		void Close(uint16 Code, const FString& Reason);

		bool IsClient() const { return bClient; }

	private:
		enum class EState : uint8 { Handshake, Open, Closing, Closed };

		void HandleHandshake();
		void ProcessFrames();
		void SendClose(uint16 Code, const FString& Reason);
		void Send(FBytes&& Bytes, FSendCallback Callback = nullptr);
		void CloseTransport();
		void RunOnStrand(std::function<void()> Function);

		const bool bClient;
		FCallbacks Callbacks;
		FBytes HandshakeBytes;
		FString Key;
		FHttpParser HandshakeParser;
		FWsFrameParser Frames;
		EState State = EState::Handshake;
		bool bCloseSent = false;
		bool bCloseReceived = false;
		bool bUserAbort = false;
		uint16 CloseCode = 1006;
		FString CloseReason = TEXT("Abnormal closure");
	};
}
