/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "udpserver.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateUdpServer);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateUdpServerMessageSent, const FErrorCode &, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FDelegateUdpServerMessage, const TArray<uint8> &, Buffer, int,  BytesRecv, const FUdpEndpoint &, Endpoint);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateUdpServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|UDP")
class INTERNETPROTOCOL_API UUDPServer : public UObject
{
	GENERATED_BODY()
public:
	UUDPServer() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|UDP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|UDP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	bool IsRooted();
	
	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	FUdpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	FUdpEndpoint RemoteEndpoint();

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void SetRecvBufferSize(int Val);

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	int GetRecvBufferSize();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	bool SendTo(const FString &Message, const FUdpEndpoint &Endpoint, const FDelegateUdpServerMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	bool SendBufferTo(const TArray<uint8> &Buffer, const FUdpEndpoint &Endpoint, const FDelegateUdpServerMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	bool Bind(const FServerBindOptions &BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|UDP|Events")
	FDelegateUdpServer OnListening;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|UDP|Events")
	FDelegateUdpServerMessage OnMessage;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|UDP|Events")
	FDelegateUdpServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|UDP|Events")
	FDelegateUdpServerError OnError;
private:
	friend struct IPNet::FFacadeAccess;
	FIpUdpState Net;
	int32 recv_buffer_size = 16384;
};
