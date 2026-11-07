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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpServerClientSslConnect, UTCPRemoteSsl*, Client);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateTcpServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPServer : public UObject
{
	GENERATED_BODY()
public:
	UTCPServer() {}
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	int Backlog = 2147483647;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	const TSet<UTCPRemote*> &Clients();

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

private:
	friend struct IPNet::FFacadeAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UTCPRemote*> ClientSet;
	void RemoveClient(UTCPRemote* Remote) { ClientSet.Remove(Remote); }
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|TCP")
class INTERNETPROTOCOL_API UTCPServerSsl : public UObject
{
	GENERATED_BODY()
public:
	UTCPServerSsl() {}

	void Construct(const FSecurityContextOpts &SecOpts);

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|TCP")
	int Backlog = 2147483647;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	const TSet<UTCPRemoteSsl*> &Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|TCP")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|TCP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServer OnListening;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServerClientSslConnect OnClientAccepted;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|TCP|Events")
	FDelegateTcpServerError OnError;

private:
	friend struct IPNet::FFacadeAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UTCPRemoteSsl*> ClientSet;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	void RemoveClient(UTCPRemoteSsl* Remote) { ClientSet.Remove(Remote); }
};