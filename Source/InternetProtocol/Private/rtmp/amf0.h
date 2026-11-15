/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include <string>
#include <vector>

/*
 * AMF0 (Action Message Format 0) as used by RTMP command and data messages.
 * The decoder is defensive: input comes from the network, so nesting depth, element counts and string
 * lengths are bounded and every read is bounds-checked. Malformed input makes Decode return false.
 */
namespace IPNet::Rtmp {

	enum class EAmf0Type : uint8 {
		Number = 0x00,
		Boolean = 0x01,
		String = 0x02,
		Object = 0x03,
		Null = 0x05,
		Undefined = 0x06,
		EcmaArray = 0x08,
		ObjectEnd = 0x09,
		StrictArray = 0x0A,
		Date = 0x0B,
		LongString = 0x0C,
	};

	struct FAmf0Value {
		EAmf0Type Type = EAmf0Type::Null;
		double Number = 0.0;
		bool Boolean = false;
		std::string String;
		/** Object / ECMA array properties, in order. Keys[i] pairs with Values[i]. */
		std::vector<std::string> Keys;
		/** Object / ECMA array property values, or StrictArray elements. */
		std::vector<FAmf0Value> Values;

		static FAmf0Value MakeNumber(double InValue);
		static FAmf0Value MakeBoolean(bool InValue);
		static FAmf0Value MakeString(std::string InValue);
		static FAmf0Value MakeNull();
		static FAmf0Value MakeObject();
		static FAmf0Value MakeEcmaArray();

		/** Object/ECMA array: appends a property and returns *this for chaining. */
		FAmf0Value& Add(std::string Key, FAmf0Value Value);
		/** Object/ECMA array lookup (case-sensitive), nullptr if absent. */
		const FAmf0Value* Find(const char* Key) const;

		bool IsNumber() const { return Type == EAmf0Type::Number || Type == EAmf0Type::Date; }
		bool IsString() const { return Type == EAmf0Type::String || Type == EAmf0Type::LongString; }
		bool IsObjectLike() const { return Type == EAmf0Type::Object || Type == EAmf0Type::EcmaArray; }

		/** Convenience accessors with defaults when the property is missing or has another type. */
		std::string GetString(const char* Key, const std::string& Default = std::string()) const;
		double GetNumber(const char* Key, double Default = 0.0) const;
	};

	struct FAmf0Limits {
		int32 MaxDepth = 16;
		size_t MaxElements = 4096;
		size_t MaxStringBytes = 1024 * 1024;
	};

	/** Appends the encoding of Value to Out. */
	void Amf0Encode(const FAmf0Value& Value, std::vector<uint8>& Out);

	/**
	 * Decodes one value starting at Pos and advances Pos. Returns false on malformed/oversized input
	 * (Pos is then unspecified).
	 */
	bool Amf0Decode(const uint8* Data, size_t Size, size_t& Pos, FAmf0Value& Out, const FAmf0Limits& Limits = FAmf0Limits());

	/** Decodes values until the end of the buffer (a command message body). */
	bool Amf0DecodeAll(const uint8* Data, size_t Size, std::vector<FAmf0Value>& Out, const FAmf0Limits& Limits = FAmf0Limits());
}
