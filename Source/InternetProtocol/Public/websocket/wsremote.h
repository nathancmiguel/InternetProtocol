/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "net/fwd.h"
#include "wsremote.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateWsRemote);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsRemoteHandshake, const FHttpRequest&, Handshake);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateWsRemoteMessageSent, const FErrorCode &, ErrorCode, int, BytesSent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateWsRemoteMessage, const TArray<uint8> &, Buffer, bool, bIsBinary);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateWsRemoteClose, int, Code, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsRemoteError, const FErrorCode&, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSRemote : public UObject
{
	GENERATED_BODY()
public:
	UWSRemote() {}

	virtual void BeginDestroy() override;

	UFUNCTION(bLueprintCallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Write(const FString &Message, const FDataframe &Dataframe, const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDataframe &Dataframe, const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Ping(const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Pong(const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void End(int code, const FString &reason);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void Close(int code, const FString &reason);

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteHandshake OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteHandshake OnUnexpectedHandshake;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemote OnPing;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemote OnPong;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteClose OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpWsState State;
	/** Set by the owning server (Game Thread). */
	TFunction<void()> OnClosedNative;
	void HandleClosedNative() { if (OnClosedNative) { OnClosedNative(); } }
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSRemoteSsl : public UObject
{
	GENERATED_BODY()
public:
	UWSRemoteSsl() {}

	virtual void BeginDestroy() override;

	UFUNCTION(bLueprintCallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Write(const FString &Message, const FDataframe &Dataframe, const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool WriteBuffer(const TArray<uint8> &Buffer, const FDataframe &Dataframe, const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Ping(const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool Pong(const FDelegateWsRemoteMessageSent &Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void End(int code, const FString &reason);

	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void Close(int code, const FString &reason);

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteHandshake OnConnected;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteHandshake OnUnexpectedHandshake;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteMessage OnMessage;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemote OnPing;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemote OnPong;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteClose OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsRemoteError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpWsState State;
	/** Set by the owning server (Game Thread). */
	TFunction<void()> OnClosedNative;
	void HandleClosedNative() { if (OnClosedNative) { OnClosedNative(); } }
};
