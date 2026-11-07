/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/websocket.h"
#include "net/http.h"
#include "Misc/Base64.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/rand.h>
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace IPNet {
	namespace {
		bool RandomBytes(uint8* Out, int32 Size) {
			if (RAND_bytes(Out, Size) == 1) {
				return true;
			}
			// OpenSSL RNG failure is extremely unlikely; fall back to the platform generator rather than zeroes.
			for (int32 Index = 0; Index < Size; ++Index) {
				Out[Index] = static_cast<uint8>(FMath::RandRange(0, 255));
			}
			return false;
		}

		bool IsValidCloseCode(uint16 Code) {
			if (Code >= 3000 && Code <= 4999) {
				return true;
			}
			switch (Code) {
			case 1000: case 1001: case 1002: case 1003: case 1007: case 1008: case 1009: case 1010: case 1011:
				return true;
			default:
				return false;
			}
		}
	}

	bool IsValidUtf8(const uint8* Data, size_t Size) {
		size_t Index = 0;
		while (Index < Size) {
			const uint8 C = Data[Index];
			size_t Extra;
			uint32 CodePoint;
			if (C < 0x80) { ++Index; continue; }
			if ((C & 0xE0) == 0xC0) { Extra = 1; CodePoint = C & 0x1F; }
			else if ((C & 0xF0) == 0xE0) { Extra = 2; CodePoint = C & 0x0F; }
			else if ((C & 0xF8) == 0xF0) { Extra = 3; CodePoint = C & 0x07; }
			else { return false; }
			if (Index + Extra >= Size) {
				return false;
			}
			for (size_t K = 1; K <= Extra; ++K) {
				const uint8 Next = Data[Index + K];
				if ((Next & 0xC0) != 0x80) return false;
				CodePoint = (CodePoint << 6) | (Next & 0x3F);
			}
			// Overlong encodings, surrogates and out-of-range code points.
			if ((Extra == 1 && CodePoint < 0x80) || (Extra == 2 && CodePoint < 0x800) || (Extra == 3 && CodePoint < 0x10000) ||
			    (CodePoint >= 0xD800 && CodePoint <= 0xDFFF) || CodePoint > 0x10FFFF) {
				return false;
			}
			Index += Extra + 1;
		}
		return true;
	}

	FString GenerateWebSocketKey() {
		uint8 Nonce[16];
		RandomBytes(Nonce, sizeof(Nonce));
		return FBase64::Encode(Nonce, sizeof(Nonce));
	}

	FString ComputeWebSocketAccept(const FString& Key) {
		const FBytes Input = ToUtf8(Key.TrimStartAndEnd() + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
		uint8 Digest[SHA_DIGEST_LENGTH];
		SHA1(Input.data(), Input.size(), Digest);
		return FBase64::Encode(Digest, SHA_DIGEST_LENGTH);
	}

	bool ValidateWsUpgradeRequest(const FHttpRequest& Request, FString& OutReason) {
		if (Request.Method != ERequestMethod::GET) {
			OutReason = TEXT("WebSocket upgrade requires GET");
			return false;
		}
		const FString* Connection = FindHeader(Request.Headers, TEXT("Connection"));
		if (!Connection || !HeaderHasToken(*Connection, TEXT("Upgrade"))) {
			OutReason = TEXT("\"Connection\" header must contain \"Upgrade\"");
			return false;
		}
		const FString* Upgrade = FindHeader(Request.Headers, TEXT("Upgrade"));
		if (!Upgrade || !HeaderHasToken(*Upgrade, TEXT("websocket"))) {
			OutReason = TEXT("\"Upgrade\" header must be \"websocket\"");
			return false;
		}
		const FString* Key = FindHeader(Request.Headers, TEXT("Sec-WebSocket-Key"));
		TArray<uint8> Decoded;
		if (!Key || !FBase64::Decode(Key->TrimStartAndEnd(), Decoded) || Decoded.Num() != 16) {
			OutReason = TEXT("missing or invalid \"Sec-WebSocket-Key\"");
			return false;
		}
		const FString* Version = FindHeader(Request.Headers, TEXT("Sec-WebSocket-Version"));
		if (!Version || Version->TrimStartAndEnd() != TEXT("13")) {
			OutReason = TEXT("\"Sec-WebSocket-Version\" must be 13");
			return false;
		}
		return true;
	}

	bool ValidateWsUpgradeResponse(const FHttpResponse& Response, const FString& SentKey, FString& OutReason) {
		if (Response.Status_Code != 101) {
			OutReason = FString::Printf(TEXT("unexpected status %d"), Response.Status_Code);
			return false;
		}
		const FString* Connection = FindHeader(Response.Headers, TEXT("Connection"));
		if (!Connection || !HeaderHasToken(*Connection, TEXT("Upgrade"))) {
			OutReason = TEXT("\"Connection\" header must contain \"Upgrade\"");
			return false;
		}
		const FString* Upgrade = FindHeader(Response.Headers, TEXT("Upgrade"));
		if (!Upgrade || !HeaderHasToken(*Upgrade, TEXT("websocket"))) {
			OutReason = TEXT("\"Upgrade\" header must be \"websocket\"");
			return false;
		}
		const FString* Accept = FindHeader(Response.Headers, TEXT("Sec-WebSocket-Accept"));
		if (!Accept || Accept->TrimStartAndEnd() != ComputeWebSocketAccept(SentKey)) {
			OutReason = TEXT("invalid \"Sec-WebSocket-Accept\"");
			return false;
		}
		return true;
	}

	FBytes EncodeWsFrame(EOpcode Opcode, const uint8* Data, size_t Size, bool bFin, bool bRsv1, bool bRsv2, bool bRsv3, bool bMask) {
		FBytes Out;
		Out.reserve(Size + 14);
		uint8 Byte1 = bFin ? 0x80 : 0x00;
		Byte1 |= bRsv1 ? static_cast<uint8>(ERSV::RSV1) : 0;
		Byte1 |= bRsv2 ? static_cast<uint8>(ERSV::RSV2) : 0;
		Byte1 |= bRsv3 ? static_cast<uint8>(ERSV::RSV3) : 0;
		Byte1 |= static_cast<uint8>(Opcode) & 0x0F;
		Out.push_back(Byte1);

		const uint8 MaskBit = bMask ? 0x80 : 0x00;
		if (Size <= 125) {
			Out.push_back(MaskBit | static_cast<uint8>(Size));
		} else if (Size <= 0xFFFF) {
			Out.push_back(MaskBit | 126);
			Out.push_back(static_cast<uint8>((Size >> 8) & 0xFF));
			Out.push_back(static_cast<uint8>(Size & 0xFF));
		} else {
			Out.push_back(MaskBit | 127);
			for (int32 Shift = 56; Shift >= 0; Shift -= 8) {
				Out.push_back(static_cast<uint8>((static_cast<uint64>(Size) >> Shift) & 0xFF));
			}
		}

		uint8 Key[4] = {0, 0, 0, 0};
		if (bMask) {
			RandomBytes(Key, 4);
			Out.insert(Out.end(), Key, Key + 4);
		}
		const size_t Offset = Out.size();
		if (Size > 0) {
			Out.insert(Out.end(), Data, Data + Size);
			if (bMask) {
				for (size_t Index = 0; Index < Size; ++Index) {
					Out[Offset + Index] ^= Key[Index & 3];
				}
			}
		}
		return Out;
	}

	FBytes EncodeWsCloseFrame(uint16 Code, const FString& Reason, bool bMask) {
		FBytes Payload;
		if (Code != 1005 && Code != 1006 && Code != 1015) {
			Payload.push_back(static_cast<uint8>(Code >> 8));
			Payload.push_back(static_cast<uint8>(Code & 0xFF));
			FBytes ReasonBytes = ToUtf8(Reason);
			// Control frame payloads are limited to 125 bytes: truncate on a UTF-8 boundary.
			size_t Max = 123;
			if (ReasonBytes.size() > Max) {
				while (Max > 0 && (ReasonBytes[Max] & 0xC0) == 0x80) {
					--Max;
				}
				ReasonBytes.resize(Max);
			}
			Payload.insert(Payload.end(), ReasonBytes.begin(), ReasonBytes.end());
		}
		return EncodeWsFrame(EOpcode::CLOSE_FRAME, Payload.data(), Payload.size(), true, false, false, false, bMask);
	}

	// ---------------------------------------------------------------------------------------------------------

	FWsFrameParser::FWsFrameParser(bool bInExpectMasked, size_t InMaxMessageBytes)
		: bExpectMasked(bInExpectMasked), MaxMessageBytes(InMaxMessageBytes) {}

	void FWsFrameParser::Append(const uint8* Data, size_t Size) {
		if (bFailed || Size == 0) {
			return;
		}
		if (Pos > 0 && Pos == Buffer.size()) {
			Buffer.clear();
			Pos = 0;
		} else if (Pos > 64 * 1024 && Pos * 2 > Buffer.size()) {
			Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos));
			Pos = 0;
		}
		Buffer.insert(Buffer.end(), Data, Data + Size);
	}

	bool FWsFrameParser::Error(FWsEvent& Out, uint16 Code, const TCHAR* Reason) {
		bFailed = true;
		Out = FWsEvent();
		Out.Type = FWsEvent::EType::ProtocolError;
		Out.Code = Code;
		Out.Reason = Reason;
		return true;
	}

	bool FWsFrameParser::Next(FWsEvent& Out) {
		while (!bFailed) {
			const size_t Available = Buffer.size() - Pos;
			if (Available < 2) {
				return false;
			}
			const uint8* Header = Buffer.data() + Pos;
			const bool bFin = (Header[0] & 0x80) != 0;
			const uint8 Opcode = Header[0] & 0x0F;
			const bool bMasked = (Header[1] & 0x80) != 0;
			uint64 Length = Header[1] & 0x7F;
			size_t HeaderSize = 2;
			if (Length == 126) {
				if (Available < 4) return false;
				Length = (static_cast<uint64>(Header[2]) << 8) | Header[3];
				HeaderSize = 4;
			} else if (Length == 127) {
				if (Available < 10) return false;
				Length = 0;
				for (int32 Index = 0; Index < 8; ++Index) {
					Length = (Length << 8) | Header[2 + Index];
				}
				HeaderSize = 10;
				if (Length >> 63) {
					return Error(Out, 1002, TEXT("invalid frame length"));
				}
			}

			if (bMasked != bExpectMasked) {
				return Error(Out, 1002, bExpectMasked ? TEXT("client frames must be masked") : TEXT("server frames must not be masked"));
			}
			const bool bControl = (Opcode & 0x08) != 0;
			const bool bKnown = Opcode == 0x0 || Opcode == 0x1 || Opcode == 0x2 || Opcode == 0x8 || Opcode == 0x9 || Opcode == 0xA;
			if (!bKnown) {
				return Error(Out, 1002, TEXT("unknown opcode"));
			}
			if (bControl && (!bFin || Length > 125)) {
				return Error(Out, 1002, TEXT("invalid control frame"));
			}
			if (!bControl && Message.size() + Length > MaxMessageBytes) {
				return Error(Out, 1009, TEXT("message too big"));
			}

			const size_t MaskSize = bMasked ? 4 : 0;
			if (Available < HeaderSize + MaskSize + Length) {
				return false;
			}
			uint8 Key[4] = {0, 0, 0, 0};
			if (bMasked) {
				FMemory::Memcpy(Key, Header + HeaderSize, 4);
			}
			const uint8* PayloadPtr = Header + HeaderSize + MaskSize;
			FBytes Payload(PayloadPtr, PayloadPtr + Length);
			if (bMasked) {
				for (size_t Index = 0; Index < Payload.size(); ++Index) {
					Payload[Index] ^= Key[Index & 3];
				}
			}
			Pos += HeaderSize + MaskSize + static_cast<size_t>(Length);

			if (bControl) {
				Out = FWsEvent();
				if (Opcode == 0x9) {
					Out.Type = FWsEvent::EType::Ping;
					Out.Payload = MoveTemp(Payload);
					return true;
				}
				if (Opcode == 0xA) {
					Out.Type = FWsEvent::EType::Pong;
					Out.Payload = MoveTemp(Payload);
					return true;
				}
				// Close
				Out.Type = FWsEvent::EType::Close;
				if (Payload.size() == 1) {
					return Error(Out, 1002, TEXT("invalid close payload"));
				}
				if (Payload.size() >= 2) {
					Out.Code = static_cast<uint16>((Payload[0] << 8) | Payload[1]);
					if (!IsValidCloseCode(Out.Code)) {
						return Error(Out, 1002, TEXT("invalid close code"));
					}
					if (!IsValidUtf8(Payload.data() + 2, Payload.size() - 2)) {
						return Error(Out, 1007, TEXT("invalid close reason"));
					}
					Out.Reason = FromUtf8(Payload.data() + 2, Payload.size() - 2);
				}
				return true;
			}

			if (Opcode == 0x0) {
				if (!bInMessage) {
					return Error(Out, 1002, TEXT("unexpected continuation frame"));
				}
			} else {
				if (bInMessage) {
					return Error(Out, 1002, TEXT("expected continuation frame"));
				}
				bInMessage = true;
				bMessageBinary = Opcode == 0x2;
				Message.clear();
			}
			Message.insert(Message.end(), Payload.begin(), Payload.end());
			if (!bFin) {
				continue;
			}

			bInMessage = false;
			if (!bMessageBinary && !IsValidUtf8(Message.data(), Message.size())) {
				return Error(Out, 1007, TEXT("invalid UTF-8 in text message"));
			}
			Out = FWsEvent();
			Out.Type = FWsEvent::EType::Message;
			Out.bBinary = bMessageBinary;
			Out.Payload = MoveTemp(Message);
			Message = FBytes();
			return true;
		}
		return false;
	}
}
