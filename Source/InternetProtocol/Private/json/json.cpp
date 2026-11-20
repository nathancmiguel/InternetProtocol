/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "json/json.h"
#include "net/common.h"

namespace {
	/** int64 beyond +-2^53 cannot round-trip through a double: keep the exact digits (still a JSON number). */
	TSharedRef<FJsonValue> MakeInt64Value(int64 Value) {
		return MakeShared<FJsonValueNumberString>(LexToString(Value));
	}

	/** Exact int64 from a number (string-backed numbers keep all digits) or from a numeric string ("123"). */
	bool ReadInt64(const TSharedPtr<FJsonValue>& Value, int64& Out) {
		if (!Value.IsValid()) {
			return false;
		}
		if (Value->Type == EJson::String) {
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (Text.IsEmpty() || !Text.IsNumeric()) {
				return false;
			}
			LexFromString(Out, *Text);
			return true;
		}
		return Value->Type == EJson::Number && Value->TryGetNumber(Out);
	}

	/** Whether Candidate is Root or (directly or deeply) contains it: storing it in Root would make a cycle. */
	bool ContainsObject(const TSharedPtr<FJsonObject>& Candidate, const FJsonObject* Root, int32 Depth = 0) {
		if (!Candidate.IsValid() || Depth > 256) {
			return false;
		}
		if (Candidate.Get() == Root) {
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Candidate->Values) {
			if (!Pair.Value.IsValid()) {
				continue;
			}
			if (Pair.Value->Type == EJson::Object && ContainsObject(Pair.Value->AsObject(), Root, Depth + 1)) {
				return true;
			}
			if (Pair.Value->Type == EJson::Array) {
				for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray()) {
					if (Item.IsValid() && Item->Type == EJson::Object && ContainsObject(Item->AsObject(), Root, Depth + 1)) {
						return true;
					}
				}
			}
		}
		return false;
	}
}

void UJavaScriptObjectNotation::BeginDestroy()
{
	UObject::BeginDestroy();
	Json.Reset();
}

bool UJavaScriptObjectNotation::ConstructJsonFromObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (Json.IsValid()) Reset();
	if (!JsonObject.IsValid()) return false;
	Json = JsonObject;
	return true;
}

TSharedPtr<FJsonObject> UJavaScriptObjectNotation::GetJsonObject()
{
	return Json;
}

void UJavaScriptObjectNotation::ConstructJson(EOutputExecPins& Output)
{
	if (Json.IsValid()) Reset();
	Json = MakeShareable(new FJsonObject());
	if (Json.IsValid())
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::ConstructJsonFromString(EOutputExecPins& Output, const FString& Data)
{
	if (Json.IsValid()) Reset();
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Data);
	if (FJsonSerializer::Deserialize(JsonReader, Json) && Json.IsValid())
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

FString UJavaScriptObjectNotation::ToString()
{
	if (!Json.IsValid())
	{
		return FString();
	}
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	return JsonString;
}

bool UJavaScriptObjectNotation::bIsValid()
{
	return Json.IsValid();
}

void UJavaScriptObjectNotation::Reset()
{
	Json.Reset();
}

bool UJavaScriptObjectNotation::HasField(const FString& FieldName)
{
	if (!Json.IsValid()) return false;
	return Json->HasField(FieldName);
}

void UJavaScriptObjectNotation::RemoveField(const FString& FieldName)
{
	if (!Json.IsValid()) return;
	Json->RemoveField(FieldName);
}

void UJavaScriptObjectNotation::SetBoolField(const FString& FieldName, const bool& Value)
{
	if (!Json.IsValid()) return;
	Json->SetBoolField(FieldName, Value);
}

void UJavaScriptObjectNotation::SetByteField(const FString& FieldName, const uint8& Value)
{
	if (!Json.IsValid()) return;
	double number = static_cast<double>(Value);
	Json->SetNumberField(FieldName, number);
}

void UJavaScriptObjectNotation::SetIntegerField(const FString& FieldName, const int Value)
{
	if (!Json.IsValid()) return;
	double number = static_cast<double>(Value);
	Json->SetNumberField(FieldName, number);
}

void UJavaScriptObjectNotation::SetInteger64Field(const FString& FieldName, const int64 Value)
{
	if (!Json.IsValid()) return;
	Json->SetField(FieldName, MakeInt64Value(Value));
}

void UJavaScriptObjectNotation::SetFloatField(const FString& FieldName, const float& Value)
{
	if (!Json.IsValid()) return;
	double d = static_cast<double>(Value);
	Json->SetNumberField(FieldName, d);
}

void UJavaScriptObjectNotation::SetStringField(const FString& FieldName, const FString& Value)
{
	if (!Json.IsValid()) return;
	Json->SetStringField(FieldName, Value);
}

void UJavaScriptObjectNotation::SetObjectField(const FString& FieldName, UJavaScriptObjectNotation* Value)
{
	if (!Json.IsValid() || Value == nullptr || !Value->GetJsonObject().IsValid()) return;
	if (ContainsObject(Value->GetJsonObject(), Json.Get()))
	{
		UE_LOG(LogAsio, Warning, TEXT("Json: SetObjectField('%s') refused: the object contains this object (cycle)."), *FieldName);
		return;
	}
	Json->SetObjectField(FieldName, Value->GetJsonObject());
}

void UJavaScriptObjectNotation::SetBoolArrayField(const FString& FieldName, const TArray<bool>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (bool Bool : Value)
	{
		JsonArray.Add(MakeShared<FJsonValueBoolean>(Bool));
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetByteArrayField(const FString& FieldName, const TArray<uint8>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (uint8 Byte : Value)
	{
		JsonArray.Add(MakeShareable(new FJsonValueNumber(Byte)));
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetIntegerArrayField(const FString& FieldName, const TArray<int>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (int Integer : Value)
	{
		JsonArray.Add(MakeShareable(new FJsonValueNumber(Integer)));
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetInteger64ArrayField(const FString& FieldName, const TArray<int64>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (int64 Integer : Value)
	{
		JsonArray.Add(MakeInt64Value(Integer));
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetFloatArrayField(const FString& FieldName, const TArray<float>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (float f : Value)
	{
		JsonArray.Add(MakeShareable(new FJsonValueNumber(f)));
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetStringArrayField(const FString& FieldName, const TArray<FString>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (FString String : Value)
	{
		TSharedPtr<FJsonValue> JsonValue = MakeShareable(new FJsonValueString(String));
		JsonArray.Add(JsonValue);
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::SetObjectArrayField(const FString& FieldName, const TArray<UJavaScriptObjectNotation*>& Value)
{
	if (!Json.IsValid()) return;
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (UJavaScriptObjectNotation* JsonObject : Value)
	{
		if (JsonObject != nullptr && JsonObject->GetJsonObject().IsValid())
		{
			if (ContainsObject(JsonObject->GetJsonObject(), Json.Get()))
			{
				UE_LOG(LogAsio, Warning, TEXT("Json: SetObjectArrayField('%s') skipped an element that contains this object (cycle)."), *FieldName);
				continue;
			}
			TSharedPtr<FJsonValue> JsonValue = MakeShareable(new FJsonValueObject(JsonObject->GetJsonObject()));
			JsonArray.Add(JsonValue);
		}
	}
	Json->SetArrayField(FieldName, JsonArray);
}

void UJavaScriptObjectNotation::TryGetBoolField(EOutputExecPins& Output, const FString& FieldName, bool& Value)
{
	Value = false;
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	if (Json->TryGetBoolField(FieldName, Value))
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetByteField(EOutputExecPins& Output, const FString& FieldName, uint8& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	int number;
	if (Json->TryGetNumberField(FieldName, number))
	{
		Value = static_cast<uint8>(number);
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetIntegerField(EOutputExecPins& Output, const FString& FieldName, int& Value)
{
	if (!Json.IsValid()) 
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	if (Json->TryGetNumberField(FieldName, Value))
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetInteger64Field(EOutputExecPins& Output, const FString& FieldName, int64& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	if (ReadInt64(Json->TryGetField(FieldName), Value))
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetFloatField(EOutputExecPins& Output, const FString& FieldName, float& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	double val;
	if (Json->TryGetNumberField(FieldName, val))
	{
		Value = static_cast<float>(val);
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetStringField(EOutputExecPins& Output, const FString& FieldName, FString& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	if (Json->TryGetStringField(FieldName, Value))
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetObjectField(EOutputExecPins& Output, const FString& FieldName, UJavaScriptObjectNotation*& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	// The pointer is owned by the JSON object: never delete it (it is not even set when the field is missing).
	const TSharedPtr<FJsonObject>* JsonObject = nullptr;
	if (!Json->TryGetObjectField(FieldName, JsonObject) || JsonObject == nullptr)
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	UJavaScriptObjectNotation* NewJson = NewObject<UJavaScriptObjectNotation>();
	if (!NewJson->ConstructJsonFromObject(*JsonObject))
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	Value = NewJson;
	Output = EOutputExecPins::Success;
}

void UJavaScriptObjectNotation::TryGetBoolArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<bool>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<bool> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			if (Val.IsValid() && Val->Type == EJson::Boolean)
			{
				data.Add(Val->AsBool());
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}
	
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetByteArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<uint8>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<uint8> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			if (Val.IsValid() && Val->Type == EJson::Number)
			{
				data.Add(static_cast<uint8>(Val->AsNumber()));
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}
	
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetIntegerArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<int>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<int> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			if (Val.IsValid() && Val->Type == EJson::Number)
			{
				data.Add(static_cast<int>(Val->AsNumber()));
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetInteger64ArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<int64>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<int64> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			int64 Number = 0;
			if (ReadInt64(Val, Number))
			{
				data.Add(Number);
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetFloatArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<float>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<float> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			if (Val.IsValid() && Val->Type == EJson::Number)
			{
				data.Add(static_cast<float>(Val->AsNumber()));
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}

	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetStringArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<FString>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	if (Json->TryGetStringArrayField(FieldName, Value))
	{
		Output = EOutputExecPins::Success;
		return;
	}
	Output = EOutputExecPins::Failure;
}

void UJavaScriptObjectNotation::TryGetObjectArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<UJavaScriptObjectNotation*>& Value)
{
	if (!Json.IsValid())
	{
		Output = EOutputExecPins::Failure;
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	if (Json->TryGetArrayField(FieldName, JsonArray))
	{
		TArray<UJavaScriptObjectNotation*> data;
		for (TSharedPtr<FJsonValue> Val : *JsonArray)
		{
			if (Val.IsValid() && Val->Type == EJson::Object)
			{
				data.Add(NewObject<UJavaScriptObjectNotation>());
				data.Last()->ConstructJsonFromObject(Val->AsObject());
			}
		}
		Value = data;
		Output = EOutputExecPins::Success;
		return;
	}

	Output = EOutputExecPins::Failure;
}
