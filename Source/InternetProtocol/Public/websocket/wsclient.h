/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "wsclient.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateWsClient);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsClientHandshake, const FHttpResponse&, Handshake);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateWsClientMessageSent, const FErrorCode &, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateWsClientMessage, const TArray<uint8> &, Buffer, bool, bIsBinary);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateWsClientClose, int, Code, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsClientError, const FErrorCode&, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSClient : public UObject
{
	GENERATED_BODY()
public:
	UWSClient() {
		Handshake.Path = "/chat";
		Handshake.Headers.Add("Connection", "Upgrade");
		Handshake.Headers.Add("Sec-WebSocket-Version", "13");
		Handshake.Headers.Add("Upgrade", "websocket");
	}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	FHttpRequest Handshake;

	UFUNCTION(bLueprintCallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Write(const FString &Message, const FDataframe &Dataframe, const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDataframe &Dataframe, const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Ping(const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Pong(const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Connect(const FClientBindOptions &BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void End(int code, const FString &reason);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void Close(int code, const FString &reason);

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientHandshake OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientHandshake OnUnexpectedHandshake;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClient OnPing;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClient OnPong;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientClose OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpWsState State;
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSClientSsl : public UObject
{
	GENERATED_BODY()
public:
	UWSClientSsl() {
		Handshake.Path = "/chat";
		Handshake.Headers.Add("Connection", "Upgrade");
		Handshake.Headers.Add("Sec-WebSocket-Version", "13");
		Handshake.Headers.Add("Upgrade", "websocket");
	}

	void Construct(const FSecurityContextOpts &SecOpts);

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	FHttpRequest Handshake;

	UFUNCTION(bLueprintCallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Write(const FString &Message, const FDataframe &Dataframe, const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDataframe &Dataframe, const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Ping(const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Pong(const FDelegateWsClientMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Connect(const FClientBindOptions &BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void End(int code, const FString &reason);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void Close(int code, const FString &reason);

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientHandshake OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientHandshake OnUnexpectedHandshake;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClient OnPing;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClient OnPong;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientClose OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsClientError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpWsState State;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
};