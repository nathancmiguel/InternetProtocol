/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "tcpremote.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateTcpRemote);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateTcpRemoteMessageSent, const FErrorCode &, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateTcpRemoteMessage, const TArray<uint8> &, Buffer, int,  BytesRecv);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpRemoteError, const FErrorCode &, ErrorCode);

UCLASS(BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPRemote : public UObject
{
	GENERATED_BODY()
public:
	UTCPRemote() = default;
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Write(const FString &Message, const FDelegateTcpRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDelegateTcpRemoteMessageSent &Callback);

	
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemote OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemoteMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemote OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemoteError OnError;

private:
	friend struct IPNet::FFacadeAccess;
	FIpStreamState Net;
	/** Set by the owning server (Game Thread). */
	TFunction<void()> OnClosedNative;
	void HandleClosedNative() { if (OnClosedNative) { OnClosedNative(); } }
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPRemoteSsl : public UObject
{
	GENERATED_BODY()
public:
	UTCPRemoteSsl() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Write(const FString &Message, const FDelegateTcpRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDelegateTcpRemoteMessageSent &Callback);

	
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemote OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemoteMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemote OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpRemoteError OnError;

private:
	friend struct IPNet::FFacadeAccess;
	FIpStreamState Net;
	/** Set by the owning server (Game Thread). */
	TFunction<void()> OnClosedNative;
	void HandleClosedNative() { if (OnClosedNative) { OnClosedNative(); } }
};
