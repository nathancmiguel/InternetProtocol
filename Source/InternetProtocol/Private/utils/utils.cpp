/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "utils/utils.h"
#include "net/common.h"

void UUtilsFunctionLibrary::JoinThreads() {
	UE_LOG(LogAsio, Warning, TEXT("InternetProtocol: JoinThreads is deprecated and does nothing (threads are managed by the plugin)."));
}

void UUtilsFunctionLibrary::StopThreads() {
	UE_LOG(LogAsio, Warning, TEXT("InternetProtocol: StopThreads is deprecated and does nothing (threads are managed by the plugin)."));
}

FString UUtilsFunctionLibrary::BufferToString(const TArray<uint8>& Value) {
	if (Value.Num() == 0) {
		return FString();
	}
	const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Value.GetData()), Value.Num());
	return FString(Converter.Length(), Converter.Get());
}

TArray<uint8> UUtilsFunctionLibrary::StringToByteArray(const FString& Value) {
	TArray<uint8> Result;
	if (Value.IsEmpty()) {
		return Result;
	}
	const FTCHARToUTF8 Converter(*Value, Value.Len());
	Result.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
	return Result;
}

TArray<FString> UUtilsFunctionLibrary::SplitString(const FString& Str, const FString& Delimiter) {
	TArray<FString> Tokens;
	if (Str.IsEmpty()) {
		return Tokens;
	}
	if (Delimiter.IsEmpty()) {
		Tokens.Add(Str);
		return Tokens;
	}
	// Empty tokens between consecutive delimiters are kept (previous behaviour).
	int32 Start = 0;
	for (;;) {
		const int32 Found = Str.Find(Delimiter, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (Found == INDEX_NONE) {
			Tokens.Add(Str.Mid(Start));
			break;
		}
		Tokens.Add(Str.Mid(Start, Found - Start));
		Start = Found + Delimiter.Len();
	}
	return Tokens;
}
