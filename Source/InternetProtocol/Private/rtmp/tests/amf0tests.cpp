/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "rtmp/amf0.h"
#include "Math/RandomStream.h"

using namespace IPNet::Rtmp;

namespace {
	constexpr EAutomationTestFlags RtmpTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	std::vector<uint8> Encode(const std::vector<FAmf0Value>& Values) {
		std::vector<uint8> Out;
		for (const FAmf0Value& Value : Values) {
			Amf0Encode(Value, Out);
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPAmf0RoundTripTest, "InternetProtocol.RTMP.AMF0.RoundTrip", RtmpTestFlags)
bool FIPAmf0RoundTripTest::RunTest(const FString& Parameters) {
	// A typical "connect" command.
	FAmf0Value Info = FAmf0Value::MakeObject();
	Info.Add("app", FAmf0Value::MakeString("live"))
	    .Add("tcUrl", FAmf0Value::MakeString("rtmp://127.0.0.1:1935/live"))
	    .Add("fpad", FAmf0Value::MakeBoolean(false))
	    .Add("audioCodecs", FAmf0Value::MakeNumber(3575.0))
	    .Add("objectEncoding", FAmf0Value::MakeNumber(0.0));
	FAmf0Value Meta = FAmf0Value::MakeEcmaArray();
	Meta.Add("width", FAmf0Value::MakeNumber(1920)).Add("framerate", FAmf0Value::MakeNumber(29.97));
	FAmf0Value Strict;
	Strict.Type = EAmf0Type::StrictArray;
	Strict.Values = {FAmf0Value::MakeNumber(1), FAmf0Value::MakeString("x"), FAmf0Value::MakeNull()};
	const std::string Long(70000, 'a');

	const std::vector<FAmf0Value> Input = {
		FAmf0Value::MakeString("connect"), FAmf0Value::MakeNumber(1.0), Info, Meta, Strict,
		FAmf0Value::MakeString(Long), FAmf0Value::MakeString("ol\xC3\xA1"),
	};
	const std::vector<uint8> Bytes = Encode(Input);

	std::vector<FAmf0Value> Output;
	if (!TestTrue(TEXT("Decodes"), Amf0DecodeAll(Bytes.data(), Bytes.size(), Output))) {
		return false;
	}
	TestEqual(TEXT("Value count"), static_cast<int32>(Output.size()), static_cast<int32>(Input.size()));
	TestTrue(TEXT("Command name"), Output[0].IsString() && Output[0].String == "connect");
	TestEqual(TEXT("Transaction id"), Output[1].Number, 1.0);
	TestTrue(TEXT("app"), Output[2].GetString("app") == "live");
	TestTrue(TEXT("tcUrl"), Output[2].GetString("tcUrl") == "rtmp://127.0.0.1:1935/live");
	TestEqual(TEXT("audioCodecs"), Output[2].GetNumber("audioCodecs"), 3575.0);
	TestTrue(TEXT("fpad false"), Output[2].Find("fpad") && !Output[2].Find("fpad")->Boolean);
	TestEqual(TEXT("ECMA width"), Output[3].GetNumber("width"), 1920.0);
	TestEqual(TEXT("ECMA framerate"), Output[3].GetNumber("framerate"), 29.97);
	TestEqual(TEXT("Strict array size"), static_cast<int32>(Output[4].Values.size()), 3);
	TestTrue(TEXT("Long string type"), Output[5].Type == EAmf0Type::LongString && Output[5].String.size() == Long.size());
	TestTrue(TEXT("UTF-8 preserved"), Output[6].String == "ol\xC3\xA1");

	// Re-encoding the decoded values must give identical bytes.
	TestTrue(TEXT("Stable re-encoding"), Encode(Output) == Bytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPAmf0MalformedTest, "InternetProtocol.RTMP.AMF0.MalformedInput", RtmpTestFlags)
bool FIPAmf0MalformedTest::RunTest(const FString& Parameters) {
	std::vector<FAmf0Value> Out;

	// Every truncation of a valid encoding must be rejected (never read past the end).
	FAmf0Value Obj = FAmf0Value::MakeObject();
	Obj.Add("key", FAmf0Value::MakeString("value")).Add("n", FAmf0Value::MakeNumber(5));
	std::vector<uint8> Valid;
	Amf0Encode(FAmf0Value::MakeString("createStream"), Valid);
	const size_t FirstValueEnd = Valid.size(); // cutting exactly here leaves one complete, valid value
	Amf0Encode(Obj, Valid);
	for (size_t Cut = 1; Cut < Valid.size(); ++Cut) {
		if (Cut == FirstValueEnd) {
			continue;
		}
		if (Amf0DecodeAll(Valid.data(), Cut, Out)) {
			AddError(FString::Printf(TEXT("Truncated input of %d bytes accepted"), static_cast<int32>(Cut)));
			break;
		}
	}

	// Unknown marker.
	const uint8 Unknown[] = {0x11, 0x00};
	TestFalse(TEXT("AMF3 switch rejected"), Amf0DecodeAll(Unknown, sizeof(Unknown), Out));

	// String claims more bytes than available.
	const uint8 Liar[] = {0x02, 0xFF, 0xFF, 'a'};
	TestFalse(TEXT("Oversized string rejected"), Amf0DecodeAll(Liar, sizeof(Liar), Out));

	// Strict array claiming 4 billion elements.
	const uint8 HugeArray[] = {0x0A, 0xFF, 0xFF, 0xFF, 0xFF};
	TestFalse(TEXT("Huge array rejected"), Amf0DecodeAll(HugeArray, sizeof(HugeArray), Out));

	// Deep nesting: 64 strict arrays of 1 element each.
	std::vector<uint8> Deep;
	for (int32 Level = 0; Level < 64; ++Level) {
		const uint8 Header[] = {0x0A, 0x00, 0x00, 0x00, 0x01};
		Deep.insert(Deep.end(), Header, Header + sizeof(Header));
	}
	Deep.push_back(0x05);
	TestFalse(TEXT("Excessive nesting rejected"), Amf0DecodeAll(Deep.data(), Deep.size(), Out));

	// Random garbage: must never crash, whatever the result.
	FRandomStream Random(1234);
	std::vector<uint8> Garbage;
	for (int32 Iteration = 0; Iteration < 2000; ++Iteration) {
		Garbage.resize(static_cast<size_t>(Random.RandRange(0, 64)));
		for (uint8& Byte : Garbage) {
			Byte = static_cast<uint8>(Random.RandRange(0, 255));
		}
		Amf0DecodeAll(Garbage.data(), Garbage.size(), Out);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
