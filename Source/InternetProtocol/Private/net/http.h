/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "net/transport.h"

/*
 * HTTP/1.x helpers shared by the HTTP client/server and the WebSocket handshake.
 * Header names are stored lower-case in parsed messages (previous behaviour) and every lookup is
 * case-insensitive. Bodies are exchanged as UTF-8; Content-Length always counts bytes.
 */
namespace IPNet {

	FBytes ToUtf8(const FString& Text);
	FString FromUtf8(const uint8* Data, size_t Size);

	/** Case-insensitive header lookup. */
	const FString* FindHeader(const TMap<FString, FString>& Headers, const TCHAR* Name);
	/** True if a comma separated header value contains Token (case-insensitive), e.g. "keep-alive, Upgrade". */
	bool HeaderHasToken(const FString& Value, const TCHAR* Token);

	FString GetHttpStatusMessage(int32 Code);
	FString RequestMethodToString(ERequestMethod Method);
	ERequestMethod StringToRequestMethod(const FString& Method);

	FString UrlEncode(const FString& Text);
	FString UrlDecode(const FString& Text);

	/** Request line + headers (+ Host, Content-Length) + body. Host must already include ":port" if needed. */
	FBytes SerializeRequest(const FHttpRequest& Request, const FString& Host);
	/** Status line + headers (+ Content-Length, Connection: close) + body unless bOmitBody (HEAD). */
	FBytes SerializeResponse(const FHttpResponse& Response, bool bOmitBody, bool bClose);

	struct FHttpLimits {
		size_t MaxHeaderBytes = 64 * 1024;
		size_t MaxBodyBytes = 64 * 1024 * 1024;
	};

	/**
	 * Incremental HTTP/1.x message parser (request or response). Feed bytes with Append(), then call
	 * Parse() until it stops returning Complete. After Complete, read Request/Response, then call
	 * Next() to start the following message (pipelined bytes are kept).
	 */
	class FHttpParser {
	public:
		enum class EMode : uint8 { Request, Response };
		enum class EResult : uint8 { NeedMore, Complete, Error };

		explicit FHttpParser(EMode InMode, const FHttpLimits& InLimits = FHttpLimits());

		void Append(const uint8* Data, size_t Size);
		EResult Parse();
		/** Connection closed by the peer: completes a response delimited by EOF. */
		EResult OnEof();
		/** Prepares the next message. Unconsumed bytes stay buffered. */
		void Next();

		/** Response mode: the request was HEAD, so the response has no body whatever its headers say. */
		void SetNoBody(bool bValue) { bNoBody = bValue; }
		/** Complete right after the headers (WebSocket upgrade). Remaining bytes are available with TakeRemaining(). */
		void SetHeadersOnly(bool bValue) { bHeadersOnly = bValue; }
		FBytes TakeRemaining();

		bool HasPartialMessage() const { return Phase != EPhase::Headers || Pos < Buffer.size(); }

		FHttpRequest Request;
		FHttpResponse Response;
		/** Request mode: HTTP status to answer with when Parse() returned Error (400, 413, 431, 501, 505). */
		int32 ErrorStatus = 400;
		FString ErrorMessage;

	private:
		enum class EPhase : uint8 { Headers, FixedBody, ChunkSize, ChunkData, ChunkDataEnd, Trailers, UntilEof, Done };

		EResult ParseHeaders();
		bool ParseStartLine(const FString& Line);
		EResult ParseBody();
		EResult Fail(int32 Status, const TCHAR* Message);
		void FinishBody();
		bool ReadLine(FString& OutLine, size_t MaxLength, bool& bOutTooLong);
		void Compact();

		EMode Mode;
		FHttpLimits Limits;
		std::vector<uint8> Buffer;
		size_t Pos = 0;
		size_t HeaderScan = 0;
		EPhase Phase = EPhase::Headers;
		uint64 Remaining = 0;
		std::vector<uint8> Body;
		bool bNoBody = false;
		bool bHeadersOnly = false;
	};

	/** Parses "a=1&b=2" into Params (URL-decoded). */
	void ParseQueryString(const FString& Query, TMap<FString, FString>& OutParams);
}
