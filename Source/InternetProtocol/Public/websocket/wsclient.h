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

	UFUNCTION(blueprintcallable, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	FHttpRequest Handshake;

	UFUNCTION(bLueprintCallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();
	/** Subprotocol agreed in the handshake (empty: none). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|Websocket")
	FString GetSubProtocol();

	/** Round trip of the last Ping answered by a Pong, in ms (-1: none yet). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|Websocket")
	float GetRoundTripTimeMs();


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
	/** Connects to "ws://host[:port]/path?query" (or wss:// with UWSClientSsl); the path goes to Handshake.Path. */
	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	bool ConnectUrl(const FString& Url);

	/** Sends a Ping after this much silence (0 = never). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	float PingIntervalSeconds = 20.0f;

	/** Closes the connection (TimedOut) after this much time without any frame from the server (0 = never). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	float IdleTimeoutSeconds = 60.0f;

	/** Connects again after an unexpected close (not after Close / End), waiting 1, 2, 4... s. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	bool bAutoReconnect = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	float ReconnectMaxDelaySeconds = 30.0f;

	/** Traffic counters of the current connection (with the last ping round trip). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|Websocket")
	FConnectionStats GetStats();


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
	

	/** Encrypts the connection with TLS, verifying the server with these options. Applies to the next connection. */
	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS (with the default, verifying options unless SetTlsOptions was called). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpWsState State;
};

/** UWSClient with TLS on (kept for existing Blueprints and code). New code: UWSClient with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSClientSsl : public UWSClient
{
	GENERATED_BODY()
public:
	UWSClientSsl() { bUseTls = true; }
};
