/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "json/json.h"
#include "UObject/Package.h"

namespace IPJsonTests {
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	UJavaScriptObjectNotation* Make(const FString& Text = FString()) {
		UJavaScriptObjectNotation* Json = NewObject<UJavaScriptObjectNotation>(GetTransientPackage());
		TEnumAsByte<EOutputExecPins> Result;
		if (Text.IsEmpty()) {
			Json->ConstructJson(Result);
		} else {
			Json->ConstructJsonFromString(Result, Text);
		}
		return Json;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPJsonMissingFieldsTest, "InternetProtocol.Json.MissingFields", IPJsonTests::Flags)
bool FIPJsonMissingFieldsTest::RunTest(const FString& Parameters) {
	UJavaScriptObjectNotation* Json = IPJsonTests::Make(TEXT("{\"a\":1}"));
	TEnumAsByte<EOutputExecPins> Result;

	// Used to delete an uninitialised pointer: crashed whenever the field was missing.
	UJavaScriptObjectNotation* Object = nullptr;
	Json->TryGetObjectField(Result, TEXT("missing"), Object);
	TestEqual(TEXT("Missing object field fails"), Result.GetValue(), EOutputExecPins::Failure);
	TestNull(TEXT("No object returned"), Object);
	Json->TryGetObjectField(Result, TEXT("a"), Object);
	TestEqual(TEXT("Number is not an object"), Result.GetValue(), EOutputExecPins::Failure);

	UJavaScriptObjectNotation* Empty = NewObject<UJavaScriptObjectNotation>(GetTransientPackage());
	TestEqual(TEXT("ToString of an unset object is empty"), Empty->ToString(), FString());

	UJavaScriptObjectNotation* Nested = IPJsonTests::Make(TEXT("{\"child\":{\"x\":2}}"));
	Nested->TryGetObjectField(Result, TEXT("child"), Object);
	TestEqual(TEXT("Existing object field"), Result.GetValue(), EOutputExecPins::Success);
	int32 X = 0;
	if (TestNotNull(TEXT("Child"), Object)) {
		Object->TryGetIntegerField(Result, TEXT("x"), X);
		TestEqual(TEXT("Child value"), X, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPJsonArraysTest, "InternetProtocol.Json.Arrays", IPJsonTests::Flags)
bool FIPJsonArraysTest::RunTest(const FString& Parameters) {
	UJavaScriptObjectNotation* Json = IPJsonTests::Make();
	TEnumAsByte<EOutputExecPins> Result;

	Json->SetBoolArrayField(TEXT("flags"), {true, false, true});
	const TArray<TSharedPtr<FJsonValue>>& Stored = Json->GetJsonObject()->GetArrayField(TEXT("flags"));
	TestTrue(TEXT("Bools written as JSON booleans"), Stored.Num() == 3 && Stored[0]->Type == EJson::Boolean);
	TArray<bool> Flags;
	Json->TryGetBoolArrayField(Result, TEXT("flags"), Flags);
	TestEqual(TEXT("Bool array read back"), Flags, TArray<bool>({true, false, true}));

	Json->SetByteArrayField(TEXT("bytes"), {1, 2, 255});
	TArray<uint8> Bytes;
	Json->TryGetByteArrayField(Result, TEXT("bytes"), Bytes);
	TestEqual(TEXT("Byte array filled"), Bytes, TArray<uint8>({1, 2, 255}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPJsonInt64Test, "InternetProtocol.Json.Int64", IPJsonTests::Flags)
bool FIPJsonInt64Test::RunTest(const FString& Parameters) {
	UJavaScriptObjectNotation* Json = IPJsonTests::Make();
	TEnumAsByte<EOutputExecPins> Result;
	const int64 Big = 9007199254740993LL; // 2^53 + 1: not representable as a double.

	Json->SetInteger64Field(TEXT("id"), Big);
	int64 Read = 0;
	Json->TryGetInteger64Field(Result, TEXT("id"), Read);
	TestEqual(TEXT("int64 kept exactly"), Read, Big);
	TestTrue(TEXT("Serialized with every digit"), Json->ToString().Contains(TEXT("9007199254740993")));

	Json->SetInteger64ArrayField(TEXT("ids"), {Big, -Big});
	TArray<int64> Ids;
	Json->TryGetInteger64ArrayField(Result, TEXT("ids"), Ids);
	TestEqual(TEXT("int64 array kept exactly"), Ids, TArray<int64>({Big, -Big}));

	// Large ids sent as strings by other services are accepted too.
	UJavaScriptObjectNotation* FromString = IPJsonTests::Make(TEXT("{\"id\":\"9007199254740993\"}"));
	Read = 0;
	FromString->TryGetInteger64Field(Result, TEXT("id"), Read);
	TestEqual(TEXT("Numeric string read as int64"), Read, Big);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPJsonCycleTest, "InternetProtocol.Json.Cycle", IPJsonTests::Flags)
bool FIPJsonCycleTest::RunTest(const FString& Parameters) {
	UJavaScriptObjectNotation* Parent = IPJsonTests::Make();
	UJavaScriptObjectNotation* Child = IPJsonTests::Make();

	Parent->SetObjectField(TEXT("self"), Parent);
	TestFalse(TEXT("Self reference refused"), Parent->HasField(TEXT("self")));

	Child->SetObjectField(TEXT("parent"), Parent);
	Parent->SetObjectField(TEXT("child"), Child);
	TestFalse(TEXT("Indirect cycle refused"), Parent->HasField(TEXT("child")));

	Parent->SetObjectArrayField(TEXT("list"), {Child});
	TArray<UJavaScriptObjectNotation*> List;
	TEnumAsByte<EOutputExecPins> Result;
	Parent->TryGetObjectArrayField(Result, TEXT("list"), List);
	TestEqual(TEXT("Cyclic array element skipped"), List.Num(), 0);
	TestFalse(TEXT("Still serializable"), Parent->ToString().IsEmpty());
	return true;
}

#endif
