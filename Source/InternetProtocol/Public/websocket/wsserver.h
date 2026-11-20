/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "websocket/wsremote.h"
#include "wsserver.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateWsServer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsServerClientConnect, UWSRemote*, Client);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsServerError, const FErrorCode &, ErrorCode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateWsServerUpgradeRequest, UWSRemote*, Client, const FHttpRequest&, Request);

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSServer : public UObject
{
	GENERATED_BODY()
public:
	UWSServer() {}
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	int Backlog = 2147483647;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	TSet<UWSRemote*> Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServer OnListening;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerClientConnect OnClientAccepted;

	/**
	 * Bound: every valid upgrade request waits for Client->AcceptUpgrade() or Client->RejectUpgrade() (e.g. to
	 * check Origin or a token). Unanswered requests are dropped after HandshakeTimeoutSeconds.
	 */
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerUpgradeRequest OnUpgradeRequest;

	/** Subprotocols supported, in order of preference; the first one the client offers is selected. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	TArray<FString> SubProtocols;
	/** Remotes send a Ping after this much silence (0 = never). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	float PingIntervalSeconds = 20.0f;

	/** Remotes are closed (TimedOut) after this much time without any frame from the client (0 = never). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	float IdleTimeoutSeconds = 60.0f;


	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerError OnError;


	/** Serves TLS with this certificate and key (Cert / Cert_Chain + Private_Key). Applies to the next Open. */
	UFUNCTION(BlueprintCallable, Category = "IP|Websocket")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS; requires SetTlsOptions with a certificate. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<TObjectPtr<UWSRemote>> ClientSet;
	void RemoveClient(UWSRemote* Remote) { ClientSet.Remove(Remote); }
};

/** UWSServer with TLS on (kept for existing Blueprints and code). New code: UWSServer with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSServerSsl : public UWSServer
{
	GENERATED_BODY()
public:
	UWSServerSsl() { bUseTls = true; }
};
