/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/amf0.h"
#include <cstring>

namespace IPNet::Rtmp {

	FAmf0Value FAmf0Value::MakeNumber(double InValue) {
		FAmf0Value V;
		V.Type = EAmf0Type::Number;
		V.Number = InValue;
		return V;
	}

	FAmf0Value FAmf0Value::MakeBoolean(bool InValue) {
		FAmf0Value V;
		V.Type = EAmf0Type::Boolean;
		V.Boolean = InValue;
		return V;
	}

	FAmf0Value FAmf0Value::MakeString(std::string InValue) {
		FAmf0Value V;
		V.Type = InValue.size() > 0xFFFF ? EAmf0Type::LongString : EAmf0Type::String;
		V.String = MoveTemp(InValue);
		return V;
	}

	FAmf0Value FAmf0Value::MakeNull() {
		return FAmf0Value();
	}

	FAmf0Value FAmf0Value::MakeObject() {
		FAmf0Value V;
		V.Type = EAmf0Type::Object;
		return V;
	}

	FAmf0Value FAmf0Value::MakeEcmaArray() {
		FAmf0Value V;
		V.Type = EAmf0Type::EcmaArray;
		return V;
	}

	FAmf0Value& FAmf0Value::Add(std::string Key, FAmf0Value Value) {
		Keys.push_back(MoveTemp(Key));
		Values.push_back(MoveTemp(Value));
		return *this;
	}

	const FAmf0Value* FAmf0Value::Find(const char* Key) const {
		if (!IsObjectLike()) {
			return nullptr;
		}
		for (size_t Index = 0; Index < Keys.size() && Index < Values.size(); ++Index) {
			if (Keys[Index] == Key) {
				return &Values[Index];
			}
		}
		return nullptr;
	}

	std::string FAmf0Value::GetString(const char* Key, const std::string& Default) const {
		const FAmf0Value* Found = Find(Key);
		return Found && Found->IsString() ? Found->String : Default;
	}

	double FAmf0Value::GetNumber(const char* Key, double Default) const {
		const FAmf0Value* Found = Find(Key);
		return Found && Found->IsNumber() ? Found->Number : Default;
	}

	namespace {
		void PutU16(std::vector<uint8>& Out, uint32 Value) {
			Out.push_back(static_cast<uint8>((Value >> 8) & 0xFF));
			Out.push_back(static_cast<uint8>(Value & 0xFF));
		}

		void PutU32(std::vector<uint8>& Out, uint32 Value) {
			for (int32 Shift = 24; Shift >= 0; Shift -= 8) {
				Out.push_back(static_cast<uint8>((Value >> Shift) & 0xFF));
			}
		}

		void PutDouble(std::vector<uint8>& Out, double Value) {
			uint64 Bits;
			static_assert(sizeof(Bits) == sizeof(Value), "double must be 64-bit");
			std::memcpy(&Bits, &Value, sizeof(Bits));
			for (int32 Shift = 56; Shift >= 0; Shift -= 8) {
				Out.push_back(static_cast<uint8>((Bits >> Shift) & 0xFF));
			}
		}

		/** Property names are UTF-8 with a 16-bit length (truncated if longer, which never happens in RTMP). */
		void PutShortString(std::vector<uint8>& Out, const std::string& Text) {
			const size_t Size = FMath::Min<size_t>(Text.size(), 0xFFFF);
			PutU16(Out, static_cast<uint32>(Size));
			Out.insert(Out.end(), Text.begin(), Text.begin() + static_cast<std::ptrdiff_t>(Size));
		}

		void PutProperties(std::vector<uint8>& Out, const FAmf0Value& Value) {
			const size_t Count = FMath::Min(Value.Keys.size(), Value.Values.size());
			for (size_t Index = 0; Index < Count; ++Index) {
				PutShortString(Out, Value.Keys[Index]);
				Amf0Encode(Value.Values[Index], Out);
			}
			// Empty key + object-end marker.
			PutU16(Out, 0);
			Out.push_back(static_cast<uint8>(EAmf0Type::ObjectEnd));
		}

		struct FReader {
			const uint8* Data;
			size_t Size;
			size_t& Pos;
			const FAmf0Limits& Limits;
			size_t Elements = 0;

			bool Has(size_t Count) const { return Pos <= Size && Size - Pos >= Count; }

			bool U8(uint8& Out) {
				if (!Has(1)) return false;
				Out = Data[Pos++];
				return true;
			}

			bool U16(uint32& Out) {
				if (!Has(2)) return false;
				Out = (static_cast<uint32>(Data[Pos]) << 8) | Data[Pos + 1];
				Pos += 2;
				return true;
			}

			bool U32(uint32& Out) {
				if (!Has(4)) return false;
				Out = (static_cast<uint32>(Data[Pos]) << 24) | (static_cast<uint32>(Data[Pos + 1]) << 16) |
				      (static_cast<uint32>(Data[Pos + 2]) << 8) | Data[Pos + 3];
				Pos += 4;
				return true;
			}

			bool Double(double& Out) {
				if (!Has(8)) return false;
				uint64 Bits = 0;
				for (int32 Index = 0; Index < 8; ++Index) {
					Bits = (Bits << 8) | Data[Pos + Index];
				}
				Pos += 8;
				std::memcpy(&Out, &Bits, sizeof(Out));
				return true;
			}

			bool Bytes(size_t Count, std::string& Out) {
				if (Count > Limits.MaxStringBytes || !Has(Count)) return false;
				Out.assign(reinterpret_cast<const char*>(Data + Pos), Count);
				Pos += Count;
				return true;
			}

			bool Properties(FAmf0Value& Out, int32 Depth) {
				for (;;) {
					uint32 KeyLength;
					if (!U16(KeyLength)) return false;
					if (KeyLength == 0) {
						uint8 Marker;
						if (!U8(Marker)) return false;
						return Marker == static_cast<uint8>(EAmf0Type::ObjectEnd);
					}
					std::string Key;
					if (!Bytes(KeyLength, Key)) return false;
					FAmf0Value Child;
					if (!Value(Child, Depth + 1)) return false;
					Out.Keys.push_back(MoveTemp(Key));
					Out.Values.push_back(MoveTemp(Child));
				}
			}

			bool Value(FAmf0Value& Out, int32 Depth) {
				if (Depth > Limits.MaxDepth || ++Elements > Limits.MaxElements) {
					return false;
				}
				uint8 Marker;
				if (!U8(Marker)) return false;
				Out = FAmf0Value();
				Out.Type = static_cast<EAmf0Type>(Marker);
				switch (Out.Type) {
				case EAmf0Type::Number:
					return Double(Out.Number);
				case EAmf0Type::Boolean: {
					uint8 Flag;
					if (!U8(Flag)) return false;
					Out.Boolean = Flag != 0;
					return true;
				}
				case EAmf0Type::String: {
					uint32 Length;
					return U16(Length) && Bytes(Length, Out.String);
				}
				case EAmf0Type::LongString: {
					uint32 Length;
					return U32(Length) && Bytes(Length, Out.String);
				}
				case EAmf0Type::Null:
				case EAmf0Type::Undefined:
					return true;
				case EAmf0Type::Object:
					return Properties(Out, Depth);
				case EAmf0Type::EcmaArray: {
					uint32 Hint; // approximate count, not trusted
					return U32(Hint) && Properties(Out, Depth);
				}
				case EAmf0Type::StrictArray: {
					uint32 Count;
					if (!U32(Count) || Count > Limits.MaxElements) return false;
					Out.Values.reserve(Count);
					for (uint32 Index = 0; Index < Count; ++Index) {
						FAmf0Value Child;
						if (!Value(Child, Depth + 1)) return false;
						Out.Values.push_back(MoveTemp(Child));
					}
					return true;
				}
				case EAmf0Type::Date: {
					uint32 TimeZone;
					return Double(Out.Number) && U16(TimeZone);
				}
				default:
					// Movieclip, reference, unsupported, recordset, XML, typed object and AMF3 switch are
					// not used by RTMP ingest/publish; reject them instead of guessing their length.
					return false;
				}
			}
		};
	}

	void Amf0Encode(const FAmf0Value& Value, std::vector<uint8>& Out) {
		switch (Value.Type) {
		case EAmf0Type::Number:
		case EAmf0Type::Date:
			Out.push_back(static_cast<uint8>(EAmf0Type::Number));
			PutDouble(Out, Value.Number);
			break;
		case EAmf0Type::Boolean:
			Out.push_back(static_cast<uint8>(EAmf0Type::Boolean));
			Out.push_back(Value.Boolean ? 1 : 0);
			break;
		case EAmf0Type::String:
		case EAmf0Type::LongString:
			if (Value.String.size() > 0xFFFF) {
				Out.push_back(static_cast<uint8>(EAmf0Type::LongString));
				PutU32(Out, static_cast<uint32>(Value.String.size()));
			} else {
				Out.push_back(static_cast<uint8>(EAmf0Type::String));
				PutU16(Out, static_cast<uint32>(Value.String.size()));
			}
			Out.insert(Out.end(), Value.String.begin(), Value.String.end());
			break;
		case EAmf0Type::Object:
			Out.push_back(static_cast<uint8>(EAmf0Type::Object));
			PutProperties(Out, Value);
			break;
		case EAmf0Type::EcmaArray:
			Out.push_back(static_cast<uint8>(EAmf0Type::EcmaArray));
			PutU32(Out, static_cast<uint32>(FMath::Min(Value.Keys.size(), Value.Values.size())));
			PutProperties(Out, Value);
			break;
		case EAmf0Type::StrictArray:
			Out.push_back(static_cast<uint8>(EAmf0Type::StrictArray));
			PutU32(Out, static_cast<uint32>(Value.Values.size()));
			for (const FAmf0Value& Element : Value.Values) {
				Amf0Encode(Element, Out);
			}
			break;
		case EAmf0Type::Undefined:
			Out.push_back(static_cast<uint8>(EAmf0Type::Undefined));
			break;
		case EAmf0Type::Null:
		default:
			Out.push_back(static_cast<uint8>(EAmf0Type::Null));
			break;
		}
	}

	bool Amf0Decode(const uint8* Data, size_t Size, size_t& Pos, FAmf0Value& Out, const FAmf0Limits& Limits) {
		if (!Data && Size > 0) {
			return false;
		}
		FReader Reader{Data, Size, Pos, Limits};
		return Reader.Value(Out, 0);
	}

	bool Amf0DecodeAll(const uint8* Data, size_t Size, std::vector<FAmf0Value>& Out, const FAmf0Limits& Limits) {
		Out.clear();
		size_t Pos = 0;
		FReader Reader{Data, Size, Pos, Limits};
		while (Pos < Size) {
			FAmf0Value Value;
			if (!Reader.Value(Value, 0)) {
				return false;
			}
			Out.push_back(MoveTemp(Value));
		}
		return true;
	}
}
