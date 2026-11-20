/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "net/transport.h"

/*
 * RFC 6455 framing.
 * The parser is incremental (frames may be split across reads or several frames may arrive in one read),
 * reassembles fragmented messages, enforces masking direction, control frame rules, UTF-8 validity of
 * text messages / close reasons and a maximum message size.
 */
namespace IPNet {

	constexpr size_t WsMaxMessageBytes = 64 * 1024 * 1024;

	struct FWsEvent {
		enum class EType : uint8 { Message, Ping, Pong, Close, ProtocolError };
		EType Type = EType::Message;
		FBytes Payload;
		bool bBinary = false;
		/** Close: received code (1005 when absent). ProtocolError: close code to send (1002, 1007, 1009). */
		uint16 Code = 1005;
		FString Reason;
	};

	class FWsFrameParser {
	public:
		/** bExpectMasked: true on the server (client frames must be masked), false on the client. */
		FWsFrameParser(bool bInExpectMasked, size_t InMaxMessageBytes = WsMaxMessageBytes);

		void Append(const uint8* Data, size_t Size);
		/** Extracts the next event; returns false when more data is needed. After a ProtocolError, stops. */
		bool Next(FWsEvent& Out);

	private:
		bool Error(FWsEvent& Out, uint16 Code, const TCHAR* Reason);

		bool bExpectMasked;
		size_t MaxMessageBytes;
		std::vector<uint8> Buffer;
		size_t Pos = 0;
		bool bInMessage = false;
		bool bMessageBinary = false;
		FBytes Message;
		bool bFailed = false;
	};

	/** Builds one frame. bMask must be true for client->server frames and false for server->client. */
	FBytes EncodeWsFrame(EOpcode Opcode, const uint8* Data, size_t Size, bool bFin, bool bRsv1, bool bRsv2, bool bRsv3, bool bMask);
	FBytes EncodeWsCloseFrame(uint16 Code, const FString& Reason, bool bMask);

	bool IsValidUtf8(const uint8* Data, size_t Size);
	/** Random 16-byte nonce, base64 (Sec-WebSocket-Key). */
	FString GenerateWebSocketKey();
	/** base64(SHA1(key + GUID)) (Sec-WebSocket-Accept). */
	FString ComputeWebSocketAccept(const FString& Key);

	/** Validates a client upgrade request; on failure OutReason explains why and OutStatus is 400 or 426. */
	bool ValidateWsUpgradeRequest(const FHttpRequest& Request, FString& OutReason, int32& OutStatus);
	/**
	 * Validates the server response against the key and the subprotocols we sent. No extension is ever
	 * requested, so a response that selects one is rejected too.
	 */
	bool ValidateWsUpgradeResponse(const FHttpResponse& Response, const FString& SentKey, const TArray<FString>& RequestedProtocols, FString& OutReason);

	/** Close codes an endpoint may send (RFC 6455 7.4 + IANA registry 1012-1014, 3000-4999). */
	bool IsSendableCloseCode(uint16 Code);

	/** Splits a comma separated header value (Sec-WebSocket-Protocol). */
	TArray<FString> SplitHeaderList(const FString& Value);
}
