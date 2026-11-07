/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/fwd.h"
#include "httpclient.generated.h"

/**
 * 
 */

DECLARE_DYNAMIC_DELEGATE_TwoParams(FDelegateHttpClientResponse, const FErrorCode &, ErrorCode, const FHttpResponse&, Response);

/** A request waiting for (or receiving) its response. Game Thread only. */
struct FIpHttpPendingRequest {
	FHttpRequest Request;
	FDelegateHttpClientResponse Callback;
};

/** Game Thread state of an HTTP client. */
struct FIpHttpClientState {
	FIpStreamState Net;
	std::shared_ptr<IPNet::FHttpParser> Parser;
	TArray<FIpHttpPendingRequest> Queue;
	bool bInFlight = false;
	/** First error reported by the current connection, used to fail the in-flight request. */
	FErrorCode ConnectionError;
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpClient : public UObject
{
	GENERATED_BODY()
public:
	UHttpClient() {}

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsRooted();
	
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	uint8 IdleTimeoutSeconds = 0;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void SetHost(const FClientBindOptions& BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Request(const FHttpRequest& Request, const FDelegateHttpClientResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Close();

private:
	friend struct IPNet::FHttpAccess;
	FIpHttpClientState State;
	FClientBindOptions bind_options;
};

UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpClientSsl : public UObject
{
	GENERATED_BODY()
public:
	UHttpClientSsl() {}

	void Construct(const FSecurityContextOpts &SecOpts);

	virtual void BeginDestroy() override;

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP")
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	uint8 IdleTimeoutSeconds = 0;
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|HTTP")
	bool IsOpen();

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void SetHost(const FClientBindOptions& BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Request(const FHttpRequest& Request, const FDelegateHttpClientResponse& Callback);

	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Close();

private:
	friend struct IPNet::FHttpAccess;
	FIpHttpClientState State;
	FClientBindOptions bind_options;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
};
