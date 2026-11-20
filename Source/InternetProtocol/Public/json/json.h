/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "net/common.h"
#include "json.generated.h"

UCLASS(Blueprintable)
class INTERNETPROTOCOL_API UJavaScriptObjectNotation : public UObject
{
	GENERATED_BODY()
protected:
	virtual void BeginDestroy() override;

public:
	bool ConstructJsonFromObject(const TSharedPtr<FJsonObject>& JsonObject);
	TSharedPtr<FJsonObject> GetJsonObject();

public:
	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void ConstructJson(EOutputExecPins& Output);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void ConstructJsonFromString(EOutputExecPins& Output, const FString& Data);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP||Json")
	FString ToString();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP||Json")
	bool bIsValid();

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void Reset();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP||Json")
	bool HasField(const FString& FieldName);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void RemoveField(const FString& FieldName);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetBoolField(const FString& FieldName, const bool& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetByteField(const FString& FieldName, const uint8& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetIntegerField(const FString& FieldName, const int Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetInteger64Field(const FString& FieldName, const int64 Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetFloatField(const FString& FieldName, const float& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetStringField(const FString& FieldName, const FString& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetObjectField(const FString& FieldName, UJavaScriptObjectNotation* Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetBoolArrayField(const FString& FieldName, const TArray<bool>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetByteArrayField(const FString& FieldName, const TArray<uint8>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetIntegerArrayField(const FString& FieldName, const TArray<int>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetInteger64ArrayField(const FString& FieldName, const TArray<int64>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetFloatArrayField(const FString& FieldName, const TArray<float>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetStringArrayField(const FString& FieldName, const TArray<FString>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json")
	void SetObjectArrayField(const FString& FieldName, const TArray<UJavaScriptObjectNotation*>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetBoolField(EOutputExecPins& Output, const FString& FieldName, bool& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetByteField(EOutputExecPins& Output, const FString& FieldName, uint8& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetIntegerField(EOutputExecPins& Output, const FString& FieldName, int& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetInteger64Field(EOutputExecPins& Output, const FString& FieldName, int64& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetFloatField(EOutputExecPins& Output, const FString& FieldName, float& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetStringField(EOutputExecPins& Output, const FString& FieldName, FString& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetObjectField(EOutputExecPins& Output, const FString& FieldName, UJavaScriptObjectNotation*& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetBoolArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<bool>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetByteArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<uint8>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetIntegerArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<int>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetInteger64ArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<int64>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetFloatArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<float>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetStringArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<FString>& Value);

	UFUNCTION(BlueprintCallable, Category = "IP||Json", meta = (ExpandEnumAsExecs = "Output"))
	void TryGetObjectArrayField(EOutputExecPins& Output, const FString& FieldName, TArray<UJavaScriptObjectNotation*>& Value);

private:
	TSharedPtr<FJsonObject> Json;
	
};
