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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateHttpServerError, const FErrorCode &, ErrorCode);

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpServer : public UObject
{
	GENERATED_BODY()
public:
	UHttpServer() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int Backlog = 2147483647;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	uint8 IdleTimeoutSeconds = 60;

	/** A request whose handler has not called Write after this many seconds is answered with 503. 0 = never. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	float ResponseTimeoutSeconds = 30.0f;

	/** Largest request line + headers accepted (431 above). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int32 MaxHeaderBytes = 64 * 1024;

	/** Largest request body accepted (413 above). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int64 MaxBodyBytes = 64 * 1024 * 1024;

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	TSet<UHttpRemote*> Clients();

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
	

	/** Serves TLS with this certificate and key (Cert / Cert_Chain + Private_Key). Applies to the next Open. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS; requires SetTlsOptions with a certificate. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FFacadeAccess;
	friend struct IPNet::FHttpAccess;
	FIpServerState Server;
	UPROPERTY(Transient)
	TSet<TObjectPtr<UHttpRemote>> ClientSet;

	TArray<TPair<FString, FDelegateHttpServerRequest>> all_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> get_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> post_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> put_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> del_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> head_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> options_cb;
	TArray<TPair<FString, FDelegateHttpServerRequest>> patch_cb;

	void RemoveClient(UHttpRemote* Remote) { ClientSet.Remove(Remote); }
	void Route(UHttpRemote* Remote, const FHttpRequest& Request);
};

/** UHttpServer with TLS on (kept for existing Blueprints and code). New code: UHttpServer with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpServerSsl : public UHttpServer
{
	GENERATED_BODY()
public:
	UHttpServerSsl() { bUseTls = true; }
};
