/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/http.h"

namespace IPNet {

	FBytes ToUtf8(const FString& Text) {
		if (Text.IsEmpty()) {
			return {};
		}
		const FTCHARToUTF8 Utf8(*Text, Text.Len());
		const uint8* Begin = reinterpret_cast<const uint8*>(Utf8.Get());
		return FBytes(Begin, Begin + Utf8.Length());
	}

	FString FromUtf8(const uint8* Data, size_t Size) {
		if (!Data || Size == 0) {
			return FString();
		}
		const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Data), static_cast<int32>(Size));
		return FString(Converter.Length(), Converter.Get());
	}

	const FString* FindHeader(const TMap<FString, FString>& Headers, const TCHAR* Name) {
		// TMap<FString,...> keys already compare case-insensitively, but be explicit about the contract.
		for (const TPair<FString, FString>& Pair : Headers) {
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase)) {
				return &Pair.Value;
			}
		}
		return nullptr;
	}

	bool HeaderHasToken(const FString& Value, const TCHAR* Token) {
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& Part : Parts) {
			if (Part.TrimStartAndEnd().Equals(Token, ESearchCase::IgnoreCase)) {
				return true;
			}
		}
		return false;
	}

	FString GetHttpStatusMessage(int32 Code) {
		static const TMap<int32, FString> Messages = {
			{100, TEXT("Continue")}, {101, TEXT("Switching Protocols")}, {102, TEXT("Processing")}, {103, TEXT("Early Hints")},
			{200, TEXT("OK")}, {201, TEXT("Created")}, {202, TEXT("Accepted")}, {203, TEXT("Non-Authoritative Information")},
			{204, TEXT("No Content")}, {205, TEXT("Reset Content")}, {206, TEXT("Partial Content")}, {207, TEXT("Multi-Status")},
			{208, TEXT("Already Reported")}, {226, TEXT("IM Used")},
			{300, TEXT("Multiple Choices")}, {301, TEXT("Moved Permanently")}, {302, TEXT("Found")}, {303, TEXT("See Other")},
			{304, TEXT("Not Modified")}, {305, TEXT("Use Proxy")}, {306, TEXT("Switch Proxy")}, {307, TEXT("Temporary Redirect")},
			{308, TEXT("Permanent Redirect")},
			{400, TEXT("Bad Request")}, {401, TEXT("Unauthorized")}, {402, TEXT("Payment Required")}, {403, TEXT("Forbidden")},
			{404, TEXT("Not Found")}, {405, TEXT("Method Not Allowed")}, {406, TEXT("Not Acceptable")},
			{407, TEXT("Proxy Authentication Required")}, {408, TEXT("Request Timeout")}, {409, TEXT("Conflict")},
			{410, TEXT("Gone")}, {411, TEXT("Length Required")}, {412, TEXT("Precondition Failed")},
			{413, TEXT("Payload Too Large")}, {414, TEXT("URI Too Long")}, {415, TEXT("Unsupported Media Type")},
			{416, TEXT("Range Not Satisfiable")}, {417, TEXT("Expectation Failed")}, {418, TEXT("I'm a teapot")},
			{421, TEXT("Misdirected Request")}, {422, TEXT("Unprocessable Entity")}, {423, TEXT("Locked")},
			{424, TEXT("Failed Dependency")}, {425, TEXT("Too Early")}, {426, TEXT("Upgrade Required")},
			{428, TEXT("Precondition Required")}, {429, TEXT("Too Many Requests")},
			{431, TEXT("Request Header Fields Too Large")}, {451, TEXT("Unavailable For Legal Reasons")},
			{500, TEXT("Internal Server Error")}, {501, TEXT("Not Implemented")}, {502, TEXT("Bad Gateway")},
			{503, TEXT("Service Unavailable")}, {504, TEXT("Gateway Timeout")}, {505, TEXT("HTTP Version Not Supported")},
			{506, TEXT("Variant Also Negotiates")}, {507, TEXT("Insufficient Storage")}, {508, TEXT("Loop Detected")},
			{510, TEXT("Not Extended")}, {511, TEXT("Network Authentication Required")},
		};
		if (const FString* Found = Messages.Find(Code)) {
			return *Found;
		}
		return TEXT("Unknown");
	}

	FString RequestMethodToString(ERequestMethod Method) {
		switch (Method) {
		case ERequestMethod::DEL: return TEXT("DELETE");
		case ERequestMethod::GET: return TEXT("GET");
		case ERequestMethod::HEAD: return TEXT("HEAD");
		case ERequestMethod::OPTIONS: return TEXT("OPTIONS");
		case ERequestMethod::PATCH: return TEXT("PATCH");
		case ERequestMethod::POST: return TEXT("POST");
		case ERequestMethod::PUT: return TEXT("PUT");
		default: return FString();
		}
	}

	ERequestMethod StringToRequestMethod(const FString& Method) {
		// Methods are case-sensitive (RFC 9110 section 9.1).
		if (Method.Equals(TEXT("GET"), ESearchCase::CaseSensitive)) return ERequestMethod::GET;
		if (Method.Equals(TEXT("POST"), ESearchCase::CaseSensitive)) return ERequestMethod::POST;
		if (Method.Equals(TEXT("PUT"), ESearchCase::CaseSensitive)) return ERequestMethod::PUT;
		if (Method.Equals(TEXT("DELETE"), ESearchCase::CaseSensitive)) return ERequestMethod::DEL;
		if (Method.Equals(TEXT("HEAD"), ESearchCase::CaseSensitive)) return ERequestMethod::HEAD;
		if (Method.Equals(TEXT("OPTIONS"), ESearchCase::CaseSensitive)) return ERequestMethod::OPTIONS;
		if (Method.Equals(TEXT("PATCH"), ESearchCase::CaseSensitive)) return ERequestMethod::PATCH;
		return ERequestMethod::UNKNOWN;
	}

	FString UrlEncode(const FString& Text) {
		static const char* Hex = "0123456789ABCDEF";
		const FBytes Utf8 = ToUtf8(Text);
		FString Out;
		Out.Reserve(static_cast<int32>(Utf8.size() * 3));
		for (const uint8 C : Utf8) {
			const bool bUnreserved = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') ||
			                         C == '-' || C == '_' || C == '.' || C == '~';
			if (bUnreserved) {
				Out.AppendChar(static_cast<TCHAR>(C));
			} else {
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(static_cast<TCHAR>(Hex[C >> 4]));
				Out.AppendChar(static_cast<TCHAR>(Hex[C & 0x0F]));
			}
		}
		return Out;
	}

	FString UrlDecode(const FString& Text) {
		const FBytes In = ToUtf8(Text);
		FBytes Out;
		Out.reserve(In.size());
		auto HexValue = [](uint8 C) -> int32 {
			if (C >= '0' && C <= '9') return C - '0';
			if (C >= 'a' && C <= 'f') return C - 'a' + 10;
			if (C >= 'A' && C <= 'F') return C - 'A' + 10;
			return -1;
		};
		for (size_t Index = 0; Index < In.size(); ++Index) {
			const uint8 C = In[Index];
			if (C == '+') {
				Out.push_back(' ');
			} else if (C == '%' && Index + 2 < In.size() && HexValue(In[Index + 1]) >= 0 && HexValue(In[Index + 2]) >= 0) {
				Out.push_back(static_cast<uint8>(HexValue(In[Index + 1]) * 16 + HexValue(In[Index + 2])));
				Index += 2;
			} else {
				Out.push_back(C);
			}
		}
		return FromUtf8(Out.data(), Out.size());
	}

	void ParseQueryString(const FString& Query, TMap<FString, FString>& OutParams) {
		TArray<FString> Pairs;
		Query.ParseIntoArray(Pairs, TEXT("&"), true);
		for (const FString& Pair : Pairs) {
			FString Key, Value;
			if (!Pair.Split(TEXT("="), &Key, &Value)) {
				Key = Pair;
			}
			OutParams.Add(UrlDecode(Key), UrlDecode(Value));
		}
	}

	namespace {
		/** Rejects CR/LF in header names/values (header injection / response splitting). */
		bool IsSafeHeaderText(const FString& Text) {
			int32 Index;
			return !Text.FindChar(TEXT('\r'), Index) && !Text.FindChar(TEXT('\n'), Index);
		}

		void AppendAscii(FBytes& Out, const FString& Text) {
			const FBytes Utf8 = ToUtf8(Text);
			Out.insert(Out.end(), Utf8.begin(), Utf8.end());
		}

		void AppendHeaders(FString& Head, const TMap<FString, FString>& Headers, TFunctionRef<bool(const FString&)> Skip) {
			for (const TPair<FString, FString>& Header : Headers) {
				if (Header.Key.IsEmpty() || Skip(Header.Key)) {
					continue;
				}
				if (!IsSafeHeaderText(Header.Key) || !IsSafeHeaderText(Header.Value)) {
					UE_LOG(LogAsio, Warning, TEXT("InternetProtocol HTTP: header '%s' dropped (contains CR/LF)."), *Header.Key);
					continue;
				}
				Head += Header.Key + TEXT(": ") + Header.Value + TEXT("\r\n");
			}
		}
	}

	FBytes SerializeRequest(const FHttpRequest& Request, const FString& Host) {
		const FBytes Body = ToUtf8(Request.Body);
		FString Target = Request.Path.IsEmpty() ? FString(TEXT("/")) : Request.Path;
		if (Request.Params.Num() > 0) {
			Target += Target.Contains(TEXT("?")) ? TEXT("&") : TEXT("?");
			bool bFirst = true;
			for (const TPair<FString, FString>& Param : Request.Params) {
				if (!bFirst) {
					Target += TEXT("&");
				}
				Target += UrlEncode(Param.Key) + TEXT("=") + UrlEncode(Param.Value);
				bFirst = false;
			}
		}
		Target.ReplaceInline(TEXT(" "), TEXT("%20"));

		const FString Version = Request.Version.IsEmpty() ? FString(TEXT("1.1")) : Request.Version;
		FString Head = RequestMethodToString(Request.Method) + TEXT(" ") + Target + TEXT(" HTTP/") + Version + TEXT("\r\n");
		if (!FindHeader(Request.Headers, TEXT("Host"))) {
			Head += TEXT("Host: ") + Host + TEXT("\r\n");
		}
		AppendHeaders(Head, Request.Headers, [](const FString& Key) {
			return Key.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase);
		});
		const bool bMethodHasBody = Request.Method == ERequestMethod::POST || Request.Method == ERequestMethod::PUT ||
		                            Request.Method == ERequestMethod::PATCH;
		if (!Body.empty() || bMethodHasBody) {
			Head += FString::Printf(TEXT("Content-Length: %llu\r\n"), static_cast<uint64>(Body.size()));
		}
		Head += TEXT("\r\n");

		FBytes Out;
		AppendAscii(Out, Head);
		Out.insert(Out.end(), Body.begin(), Body.end());
		return Out;
	}

	FBytes SerializeResponse(const FHttpResponse& Response, bool bOmitBody, bool bClose) {
		const FBytes Body = ToUtf8(Response.Body);
		const FString Version = Response.Version.IsEmpty() ? FString(TEXT("1.1")) : Response.Version;
		const FString Message = Response.Status_Message.IsEmpty() ? GetHttpStatusMessage(Response.Status_Code) : Response.Status_Message;
		FString Head = FString::Printf(TEXT("HTTP/%s %d "), *Version, Response.Status_Code) + Message + TEXT("\r\n");
		AppendHeaders(Head, Response.Headers, [](const FString& Key) {
			return Key.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("Connection"), ESearchCase::IgnoreCase) ||
			       Key.Equals(TEXT("Transfer-Encoding"), ESearchCase::IgnoreCase);
		});
		const bool bStatusAllowsBody = !(Response.Status_Code / 100 == 1 || Response.Status_Code == 204 || Response.Status_Code == 304);
		if (bStatusAllowsBody) {
			Head += FString::Printf(TEXT("Content-Length: %llu\r\n"), static_cast<uint64>(Body.size()));
		}
		if (const FString* Connection = FindHeader(Response.Headers, TEXT("Connection"))) {
			if (!bClose && IsSafeHeaderText(*Connection)) {
				Head += TEXT("Connection: ") + *Connection + TEXT("\r\n");
			}
		}
		if (bClose) {
			Head += TEXT("Connection: close\r\n");
		}
		Head += TEXT("\r\n");

		FBytes Out;
		AppendAscii(Out, Head);
		if (!bOmitBody && bStatusAllowsBody) {
			Out.insert(Out.end(), Body.begin(), Body.end());
		}
		return Out;
	}

	// ---------------------------------------------------------------------------------------------------------
	// FHttpParser

	FHttpParser::FHttpParser(EMode InMode, const FHttpLimits& InLimits) : Mode(InMode), Limits(InLimits) {}

	void FHttpParser::Append(const uint8* Data, size_t Size) {
		if (Size > 0) {
			Buffer.insert(Buffer.end(), Data, Data + Size);
		}
	}

	FBytes FHttpParser::TakeRemaining() {
		FBytes Out(Buffer.begin() + static_cast<std::ptrdiff_t>(Pos), Buffer.end());
		Buffer.clear();
		Pos = 0;
		HeaderScan = 0;
		return Out;
	}

	void FHttpParser::Compact() {
		if (Pos > 0) {
			Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos));
			HeaderScan = HeaderScan > Pos ? HeaderScan - Pos : 0;
			Pos = 0;
		}
	}

	void FHttpParser::Next() {
		Compact();
		Request = FHttpRequest();
		Response = FHttpResponse();
		Body.clear();
		Remaining = 0;
		Phase = EPhase::Headers;
		HeaderScan = 0;
		ErrorStatus = 400;
		ErrorMessage.Reset();
	}

	FHttpParser::EResult FHttpParser::Fail(int32 Status, const TCHAR* Message) {
		ErrorStatus = Status;
		ErrorMessage = Message;
		Phase = EPhase::Done;
		return EResult::Error;
	}

	FHttpParser::EResult FHttpParser::Parse() {
		if (Phase == EPhase::Done) {
			return EResult::NeedMore;
		}
		if (Phase == EPhase::Headers) {
			const EResult Result = ParseHeaders();
			if (Result != EResult::Complete) {
				return Result;
			}
			if (Phase == EPhase::Done) {
				return EResult::Complete;
			}
		}
		return ParseBody();
	}

	FHttpParser::EResult FHttpParser::OnEof() {
		if (Phase == EPhase::UntilEof) {
			FinishBody();
			return EResult::Complete;
		}
		return Phase == EPhase::Headers && Pos >= Buffer.size() ? EResult::NeedMore : Fail(400, TEXT("connection closed mid-message"));
	}

	FHttpParser::EResult FHttpParser::ParseHeaders() {
		// Find the end of the header block.
		const size_t Start = FMath::Max(Pos, HeaderScan >= 3 ? HeaderScan - 3 : Pos);
		size_t End = std::string::npos;
		for (size_t Index = Start; Index + 3 < Buffer.size(); ++Index) {
			if (Buffer[Index] == '\r' && Buffer[Index + 1] == '\n' && Buffer[Index + 2] == '\r' && Buffer[Index + 3] == '\n') {
				End = Index;
				break;
			}
		}
		if (End == std::string::npos) {
			HeaderScan = Buffer.size();
			if (Buffer.size() - Pos > Limits.MaxHeaderBytes) {
				return Fail(431, TEXT("header block too large"));
			}
			return EResult::NeedMore;
		}
		if (End - Pos > Limits.MaxHeaderBytes) {
			return Fail(431, TEXT("header block too large"));
		}

		// Header bytes are ASCII/Latin-1 in practice; decode as UTF-8 to be tolerant.
		const FString Block = FromUtf8(Buffer.data() + Pos, End - Pos);
		Pos = End + 4;
		HeaderScan = Pos;

		TArray<FString> Lines;
		Block.ParseIntoArray(Lines, TEXT("\r\n"), false);
		if (Lines.Num() == 0 || !ParseStartLine(Lines[0])) {
			return EResult::Error;
		}

		TMap<FString, FString>& Headers = Mode == EMode::Request ? Request.Headers : Response.Headers;
		for (int32 Index = 1; Index < Lines.Num(); ++Index) {
			const FString& Line = Lines[Index];
			if (Line.IsEmpty()) {
				continue;
			}
			int32 Colon;
			if (!Line.FindChar(TEXT(':'), Colon) || Colon == 0) {
				return Fail(400, TEXT("malformed header line"));
			}
			const FString Key = Line.Left(Colon).TrimStartAndEnd().ToLower();
			const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
			if (FString* Existing = Headers.Find(Key)) {
				*Existing += TEXT(", ") + Value;
			} else {
				Headers.Add(Key, Value);
			}
		}

		// Body framing (RFC 9112 section 6).
		const FString* TransferEncoding = FindHeader(Headers, TEXT("transfer-encoding"));
		const FString* ContentLength = FindHeader(Headers, TEXT("content-length"));
		const bool bChunked = TransferEncoding && HeaderHasToken(*TransferEncoding, TEXT("chunked"));

		bool bNoBodyForThisMessage = bHeadersOnly;
		if (Mode == EMode::Response) {
			const int32 Code = Response.Status_Code;
			bNoBodyForThisMessage |= bNoBody || Code / 100 == 1 || Code == 204 || Code == 304;
		}

		if (bNoBodyForThisMessage) {
			Phase = EPhase::Done;
			FinishBody();
			return EResult::Complete;
		}
		if (bChunked) {
			if (ContentLength) {
				// Ambiguous framing is a request smuggling vector.
				return Fail(400, TEXT("both Transfer-Encoding and Content-Length present"));
			}
			Phase = EPhase::ChunkSize;
			return EResult::Complete;
		}
		if (TransferEncoding && Mode == EMode::Request) {
			return Fail(501, TEXT("unsupported Transfer-Encoding"));
		}
		if (ContentLength) {
			bool bDigitsOnly = !ContentLength->IsEmpty() && ContentLength->Len() <= 19;
			for (const TCHAR C : *ContentLength) {
				bDigitsOnly &= C >= TEXT('0') && C <= TEXT('9');
			}
			if (!bDigitsOnly) {
				return Fail(400, TEXT("invalid Content-Length"));
			}
			const uint64 Length = FCString::Strtoui64(**ContentLength, nullptr, 10);
			if (Length > Limits.MaxBodyBytes) {
				return Fail(413, TEXT("body too large"));
			}
			Remaining = Length;
			Phase = Length == 0 ? EPhase::Done : EPhase::FixedBody;
			if (Phase == EPhase::Done) {
				FinishBody();
			}
			return EResult::Complete;
		}
		if (Mode == EMode::Response) {
			Phase = EPhase::UntilEof;
			return EResult::Complete;
		}
		Phase = EPhase::Done;
		FinishBody();
		return EResult::Complete;
	}

	bool FHttpParser::ParseStartLine(const FString& Line) {
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(" "), true);
		if (Mode == EMode::Request) {
			if (Parts.Num() != 3) {
				Fail(400, TEXT("malformed request line"));
				return false;
			}
			if (!Parts[2].StartsWith(TEXT("HTTP/"), ESearchCase::CaseSensitive)) {
				Fail(400, TEXT("malformed request line"));
				return false;
			}
			Request.Version = Parts[2].Mid(5);
			if (Request.Version != TEXT("1.0") && Request.Version != TEXT("1.1")) {
				Fail(505, TEXT("HTTP version not supported"));
				return false;
			}
			Request.Method = StringToRequestMethod(Parts[0]);
			FString Path = Parts[1];
			FString Query;
			if (Path.Split(TEXT("?"), &Path, &Query)) {
				ParseQueryString(Query, Request.Params);
			}
			Request.Path = UrlDecode(Path.Replace(TEXT("+"), TEXT("%2B")));
			if (Request.Path.IsEmpty()) {
				Request.Path = TEXT("/");
			}
			return true;
		}

		if (Parts.Num() < 2 || !Parts[0].StartsWith(TEXT("HTTP/"), ESearchCase::CaseSensitive) || !Parts[1].IsNumeric()) {
			Fail(400, TEXT("malformed status line"));
			return false;
		}
		Response.Version = Parts[0].Mid(5);
		Response.Status_Code = FCString::Atoi(*Parts[1]);
		FString Message;
		for (int32 Index = 2; Index < Parts.Num(); ++Index) {
			Message += (Index > 2 ? TEXT(" ") : TEXT("")) + Parts[Index];
		}
		Response.Status_Message = Message;
		return true;
	}

	bool FHttpParser::ReadLine(FString& OutLine, size_t MaxLength, bool& bOutTooLong) {
		bOutTooLong = false;
		for (size_t Index = Pos; Index + 1 < Buffer.size(); ++Index) {
			if (Buffer[Index] == '\r' && Buffer[Index + 1] == '\n') {
				OutLine = FromUtf8(Buffer.data() + Pos, Index - Pos);
				Pos = Index + 2;
				return true;
			}
			if (Index - Pos > MaxLength) {
				bOutTooLong = true;
				return false;
			}
		}
		bOutTooLong = Buffer.size() - Pos > MaxLength;
		return false;
	}

	FHttpParser::EResult FHttpParser::ParseBody() {
		for (;;) {
			switch (Phase) {
			case EPhase::FixedBody:
			case EPhase::ChunkData: {
				const size_t Available = Buffer.size() - Pos;
				const size_t Take = static_cast<size_t>(FMath::Min<uint64>(Remaining, Available));
				Body.insert(Body.end(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos + Take));
				Pos += Take;
				Remaining -= Take;
				if (Remaining > 0) {
					Compact();
					return EResult::NeedMore;
				}
				if (Phase == EPhase::FixedBody) {
					Phase = EPhase::Done;
					FinishBody();
					return EResult::Complete;
				}
				Phase = EPhase::ChunkDataEnd;
				break;
			}
			case EPhase::ChunkDataEnd: {
				if (Buffer.size() - Pos < 2) {
					return EResult::NeedMore;
				}
				if (Buffer[Pos] != '\r' || Buffer[Pos + 1] != '\n') {
					return Fail(400, TEXT("malformed chunk"));
				}
				Pos += 2;
				Phase = EPhase::ChunkSize;
				break;
			}
			case EPhase::ChunkSize: {
				FString Line;
				bool bTooLong;
				if (!ReadLine(Line, 1024, bTooLong)) {
					return bTooLong ? Fail(400, TEXT("chunk size line too long")) : EResult::NeedMore;
				}
				int32 Semicolon;
				if (Line.FindChar(TEXT(';'), Semicolon)) {
					Line.LeftInline(Semicolon);
				}
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.Len() > 15) {
					return Fail(400, TEXT("invalid chunk size"));
				}
				uint64 Size = 0;
				for (const TCHAR C : Line) {
					int32 Digit;
					if (C >= '0' && C <= '9') Digit = C - '0';
					else if (C >= 'a' && C <= 'f') Digit = C - 'a' + 10;
					else if (C >= 'A' && C <= 'F') Digit = C - 'A' + 10;
					else return Fail(400, TEXT("invalid chunk size"));
					Size = Size * 16 + static_cast<uint64>(Digit);
				}
				if (Body.size() + Size > Limits.MaxBodyBytes) {
					return Fail(413, TEXT("body too large"));
				}
				if (Size == 0) {
					Phase = EPhase::Trailers;
				} else {
					Remaining = Size;
					Phase = EPhase::ChunkData;
				}
				break;
			}
			case EPhase::Trailers: {
				FString Line;
				bool bTooLong;
				if (!ReadLine(Line, Limits.MaxHeaderBytes, bTooLong)) {
					return bTooLong ? Fail(431, TEXT("trailers too large")) : EResult::NeedMore;
				}
				if (Line.IsEmpty()) {
					Phase = EPhase::Done;
					FinishBody();
					return EResult::Complete;
				}
				break; // trailers are ignored
			}
			case EPhase::UntilEof: {
				const size_t Available = Buffer.size() - Pos;
				if (Body.size() + Available > Limits.MaxBodyBytes) {
					return Fail(413, TEXT("body too large"));
				}
				Body.insert(Body.end(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos), Buffer.end());
				Pos = Buffer.size();
				Compact();
				return EResult::NeedMore;
			}
			default:
				return EResult::NeedMore;
			}
		}
	}

	void FHttpParser::FinishBody() {
		Phase = EPhase::Done;
		FString& Target = Mode == EMode::Request ? Request.Body : Response.Body;
		Target = FromUtf8(Body.data(), Body.size());
		Body.clear();
		Body.shrink_to_fit();
	}
}
