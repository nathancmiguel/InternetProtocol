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
	/** Status line + headers only, for a streamed response (chunked, or until the connection closes). */
	FBytes SerializeResponseHead(const FHttpResponse& Response, bool bChunked, bool bClose);
	/** One chunk of a chunked body; an empty chunk ends the body. */
	FBytes EncodeChunk(const uint8* Data, size_t Size);

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
		/** Request mode: called when the headers of a request with a body are parsed (before the body). */
		std::function<void(const FHttpRequest&)> OnRequestHeaders;

		/** Complete right after the headers (WebSocket upgrade). Remaining bytes are available with TakeRemaining(). */
		void SetHeadersOnly(bool bValue) { bHeadersOnly = bValue; }
		FBytes TakeRemaining();

		bool HasPartialMessage() const { return Phase != EPhase::Headers || Pos < Buffer.size(); }
		/** Download progress of the current message: body bytes so far and Content-Length (-1 unknown). */
		size_t BodyBytesSoFar() const { return Body.size(); }
		int64 ExpectedBodyBytes() const { return ExpectedLength; }
		/** Bytes received and not parsed yet (pipelined requests waiting for the current response). */
		size_t BufferedBytes() const { return Buffer.size() - Pos; }

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
		/** Chunked trailers seen for the current message (bounded like headers). */
		size_t TrailerBytes = 0;
		int32 TrailerLines = 0;
		EPhase Phase = EPhase::Headers;
		uint64 Remaining = 0;
		std::vector<uint8> Body;
		bool bNoBody = false;
		bool bHeadersOnly = false;
		int64 ExpectedLength = -1;
	};

	struct FParsedHttpUrl {
		FString Scheme;
		FString Host;
		int32 Port = 0;
		/** Path and query, at least "/". */
		FString Target = TEXT("/");
	};

	/** "scheme://host[:port][/path][?query]" (IPv6 hosts in brackets). Default ports: 80 / 443 (ws: 80, wss: 443). */
	bool ParseHttpUrl(const FString& Url, FParsedHttpUrl& Out);

	/** Parses "a=1&b=2" into Params (URL-decoded). */
	void ParseQueryString(const FString& Query, TMap<FString, FString>& OutParams);
}
