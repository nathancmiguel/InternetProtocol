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

	UFUNCTION(blueprintcallable, Category = "IP|UDP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|UDP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();
	
	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	FUdpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|UDP")
	FUdpEndpoint RemoteEndpoint();

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void SetRecvBufferSize(int Val);
	/** Allows sending to broadcast addresses (e.g. 255.255.255.255 or 192.168.1.255). */
	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void SetBroadcast(bool bEnable);

	/** Joins a multicast group (e.g. "239.0.0.1" or "ff02::1"); errors are reported through OnError. */
	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	bool JoinMulticastGroup(const FString& GroupAddress);

	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	bool LeaveMulticastGroup(const FString& GroupAddress);

	/** Hops of outgoing multicast datagrams (1 = local network only). */
	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void SetMulticastTTL(int32 Hops);

	/** Whether multicast datagrams sent by this host are received by it too. */
	UFUNCTION(BlueprintCallable, Category = "IP|UDP")
	void SetMulticastLoopback(bool bEnable);


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
	int32 recv_buffer_size = 65536;
};
