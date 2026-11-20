/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "tcp/tcpremote.h"
#include "tcpserver.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateTcpServer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpServerClientConnect, UTCPRemote*, Client);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPServer : public UObject
{
	GENERATED_BODY()
public:
	UTCPServer() {}
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	int Backlog = 2147483647;

	/** How the byte stream is cut into messages (applies to the next Connect / to accepted connections). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	FTcpFramingOptions Framing;


	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	TSet<UTCPRemote*> Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServer OnListening;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServerClientConnect OnClientAccepted;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServerError OnError;


	/** Serves TLS with this certificate and key (Cert / Cert_Chain + Private_Key). Applies to the next Open. */
	UFUNCTION(BlueprintCallable, Category = "IP|TCP")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS; requires SetTlsOptions with a certificate. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FFacadeAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<TObjectPtr<UTCPRemote>> ClientSet;
	void RemoveClient(UTCPRemote* Remote) { ClientSet.Remove(Remote); }
};

/** UTCPServer with TLS on (kept for existing Blueprints and code). New code: UTCPServer with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPServerSsl : public UTCPServer
{
	GENERATED_BODY()
public:
	UTCPServerSsl() { bUseTls = true; }
};
