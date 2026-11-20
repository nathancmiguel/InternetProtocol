/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "httpremote.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateHttpRemote);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateHttpRemoteMessageSent, const FErrorCode&, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateHttpRemoteError, const FErrorCode&, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpRemote : public UObject
{
	GENERATED_BODY()
public:
	UHttpRemote() {}

	virtual void BeginDestroy() override;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	FHttpResponse Headers;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	bool Write(const FDelegateHttpRemoteMessageSent &Callback);
	/**
	 * Starts a streamed response with the current Headers (status + headers, no Content-Length): the body is
	 * sent with WriteChunk / WriteChunkBuffer and finished with EndStream (chunked encoding for HTTP/1.1).
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool BeginStream();

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool WriteChunk(const FString& Data);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool WriteChunkBuffer(const TArray<uint8>& Data);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool EndStream();


	
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Close();

	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpRemote OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpRemoteError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FHttpAccess;
	FIpStreamState Net;
	std::shared_ptr<IPNet::FHttpServerSession> Session;
	bool bResponsePending = false;
	/** Incremented for every request; the automatic 503 only answers the request it was armed for. */
	uint32 ResponseSerial = 0;
	/** BeginStream was called: chunks go out until EndStream. */
	bool bStreaming = false;
	bool bStreamChunked = false;
	bool bWillClose = false;
	bool bHeadRequest = false;
	/** Set by the owning server (Game Thread). */
	TFunction<void(const FHttpRequest&)> OnRequestNative;
	TFunction<void()> OnClosedNative;
};

