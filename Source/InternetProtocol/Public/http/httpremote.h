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
	bool bWillClose = false;
	bool bHeadRequest = false;
	/** Set by the owning server (Game Thread). */
	TFunction<void(const FHttpRequest&)> OnRequestNative;
	TFunction<void()> OnClosedNative;
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpRemoteSsl : public UObject
{
	GENERATED_BODY()
public:
	UHttpRemoteSsl() {}

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
	bool bWillClose = false;
	bool bHeadRequest = false;
	/** Set by the owning server (Game Thread). */
	TFunction<void(const FHttpRequest&)> OnRequestNative;
	TFunction<void()> OnClosedNative;
};
