/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "utils.generated.h"

/**
 * 
 */


UCLASS(Blueprintable, BlueprintType, Category = "IP|Utils")
class INTERNETPROTOCOL_API UUtilsFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Deprecated: the I/O threads are owned by the module and stopped at shutdown. Does nothing. */
	UFUNCTION(BlueprintCallable, Category = "IP|Utils|Threads", meta = (DeprecatedFunction, DeprecationMessage = "I/O threads are managed by the plugin; this function does nothing."))
	static void JoinThreads();

	/** Deprecated: stopping the shared I/O threads would break every connection. Does nothing. */
	UFUNCTION(BlueprintCallable, Category = "IP|Utils|Threads", meta = (DeprecatedFunction, DeprecationMessage = "I/O threads are managed by the plugin; this function does nothing."))
	static void StopThreads();

	/*Buffer functions*/
	/** Decodes UTF-8 bytes. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="IP|Utils|Buffer")
	static FString BufferToString(const TArray<uint8>& Value);

	/** Encodes the string as UTF-8 (inverse of BufferToString). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="IP|Utils|Buffer")
	static TArray<uint8> StringToByteArray(const FString& Value);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="IP|Utils|Buffer")
	static TArray<FString> SplitString(const FString& Str, const FString& Delimiter);
};
