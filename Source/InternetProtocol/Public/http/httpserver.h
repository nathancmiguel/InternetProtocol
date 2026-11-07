/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "http/httpremote.h"
#include "httpserver.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegatHttpServer);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateHttpServerRequest, const FHttpRequest&, Request, UHttpRemote*, Response);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateHttpServerRequestSsl, const FHttpRequest&, Request, UHttpRemoteSsl*, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateHttpServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpServer : public UObject
{
	GENERATED_BODY()
public:
	UHttpServer() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int Backlog = 2147483647;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	uint8 IdleTimeoutSeconds = 0;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	TSet<UHttpRemote*> &Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void All(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Get(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Post(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Put(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Del(const FString &Path, const FDelegateHttpServerRequest &Callback);
	
	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
    void Head(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Options(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Patch(const FString &Path, const FDelegateHttpServerRequest &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegatHttpServer OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpServerError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FHttpAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UHttpRemote*> ClientSet;

	TMap<FString, FDelegateHttpServerRequest> all_cb;
	TMap<FString, FDelegateHttpServerRequest> get_cb;
	TMap<FString, FDelegateHttpServerRequest> post_cb;
	TMap<FString, FDelegateHttpServerRequest> put_cb;
	TMap<FString, FDelegateHttpServerRequest> del_cb;
	TMap<FString, FDelegateHttpServerRequest> head_cb;
	TMap<FString, FDelegateHttpServerRequest> options_cb;
	TMap<FString, FDelegateHttpServerRequest> patch_cb;

	void RemoveClient(UHttpRemote* Remote) { ClientSet.Remove(Remote); }
	void Route(UHttpRemote* Remote, const FHttpRequest& Request);
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpServerSsl : public UObject
{
	GENERATED_BODY()
public:
	UHttpServerSsl() {}

	void Construct(const FSecurityContextOpts &SecOpts);

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int Backlog = 2147483647;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	uint8 IdleTimeoutSeconds = 0;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	TSet<UHttpRemoteSsl*> &Clients();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FErrorCode GetErrorCode();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void All(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Get(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Post(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Put(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Del(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);
	
	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
    void Head(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Options(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Patch(const FString &Path, const FDelegateHttpServerRequestSsl &Callback);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	bool Open(const FServerBindOptions &BindOpts);

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void Close();

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegatHttpServer OnClose;

	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpServerError OnError;
	
private:
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FHttpAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<UHttpRemoteSsl*> ClientSet;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;

	TMap<FString, FDelegateHttpServerRequestSsl> all_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> get_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> post_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> put_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> del_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> head_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> options_cb;
	TMap<FString, FDelegateHttpServerRequestSsl> patch_cb;

	void RemoveClient(UHttpRemoteSsl* Remote) { ClientSet.Remove(Remote); }
	void Route(UHttpRemoteSsl* Remote, const FHttpRequest& Request);
};
