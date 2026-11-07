/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "tcpclient.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateTcpClient);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateTcpClientMessageSent, const FErrorCode &, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateTcpClientMessage, const TArray<uint8> &, Buffer, int,  BytesRecv);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpClientError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPClient : public UObject
{
	GENERATED_BODY()
public:
	UTCPClient() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsRooted();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Write(const FString &Message, const FDelegateTcpClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDelegateTcpClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Connect(const FClientBindOptions &BindOpts);
	
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClient OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClientMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClient OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClientError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	FIpStreamState Net;
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPClientSsl : public UObject
{
	GENERATED_BODY()
public:
	UTCPClientSsl() {}

	void Construct(const FSecurityContextOpts &SecOpts);

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsRooted();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Write(const FString &Message, const FDelegateTcpClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDelegateTcpClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	bool Connect(const FClientBindOptions &BindOpts);
	
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClient OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClientMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClient OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpClientError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	FIpStreamState Net;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
};
