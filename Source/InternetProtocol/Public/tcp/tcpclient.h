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

	UFUNCTION(blueprintcallable, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();


	/** How the byte stream is cut into messages (applies to the next Connect / to accepted connections). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	FTcpFramingOptions Framing;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint RemoteEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();
	/** Traffic counters of the current connection. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|TCP")
	FConnectionStats GetStats();


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
	

	/** Encrypts the connection with TLS, verifying the server with these options. Applies to the next connection. */
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS (with the default, verifying options unless SetTlsOptions was called). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FFacadeAccess;
	FIpStreamState Net;
};

/** UTCPClient with TLS on (kept for existing Blueprints and code). New code: UTCPClient with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPClientSsl : public UTCPClient
{
	GENERATED_BODY()
public:
	UTCPClientSsl() { bUseTls = true; }
};
