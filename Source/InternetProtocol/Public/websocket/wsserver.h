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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsServerClientSslConnect, UWSRemoteSsl*, Client);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateWsServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSServer : public UObject
{
	GENERATED_BODY()
public:
	UWSServer() {}
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	int Backlog = 2147483647;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	const TSet<UWSRemote*> &Clients();

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

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerError OnError;

private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UWSRemote*> ClientSet;
	void RemoveClient(UWSRemote* Remote) { ClientSet.Remove(Remote); }
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|Websocket")
class INTERNETPROTOCOL_API UWSServerSsl : public UObject
{
	GENERATED_BODY()
public:
	UWSServerSsl() {}

	void Construct(const FSecurityContextOpts &SecOpts);
	
	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|Websocket")
	int Backlog = 2147483647;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	const TSet<UWSRemoteSsl*> &Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|Websocket")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|Websocket")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServer OnListening;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerClientSslConnect OnClientAccepted;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServer OnClose;
	
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|Websocket|Events")
	FDelegateWsServerError OnError;

private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FWsAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UWSRemoteSsl*> ClientSet;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	void RemoveClient(UWSRemoteSsl* Remote) { ClientSet.Remove(Remote); }
};
