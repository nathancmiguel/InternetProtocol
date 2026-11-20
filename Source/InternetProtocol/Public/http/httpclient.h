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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FDelegateHttpClientProgress, int32, RequestId, int64, BytesReceived, int64, TotalBytes);

struct FIpHttpPendingRequest {
	FHttpRequest Request;
	FDelegateHttpClientResponse Callback;
	/** Already resent once after a stale keep-alive connection closed. */
	bool bRetried = false;
	int32 Id = 0;
	int32 Redirects = 0;
};

/** Game Thread state of an HTTP client. */
struct FIpHttpClientState {
	FIpStreamState Net;
	std::shared_ptr<IPNet::FHttpParser> Parser;
	TArray<FIpHttpPendingRequest> Queue;
	bool bInFlight = false;
	int32 NextRequestId = 0;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	bool bTls = false;
	/** Responses already received on the current connection (> 0: the connection is a reused keep-alive). */
	int32 ResponsesOnConnection = 0;
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

	/** Sends the request (queued behind earlier ones) and returns its id for Cancel. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	int32 Request(const FHttpRequest& Request, const FDelegateHttpClientResponse& Callback);

	/** Cancels a queued or in-flight request: its callback receives the Cancelled error. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool Cancel(int32 RequestId);

	/** Sets the host from a URL ("http://host[:port]/..."); the path part is not used (set FHttpRequest.Path). */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool SetUrl(const FString& Url);

	/** Follows up to this many redirects (301, 302, 303, 307, 308) on the same host. 0 = deliver them as responses. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int32 MaxRedirects = 0;

	/** Download progress of the current request: bytes received and Content-Length (-1 if unknown). */
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpClientProgress OnProgress;


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

	/** Sends the request (queued behind earlier ones) and returns its id for Cancel. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	int32 Request(const FHttpRequest& Request, const FDelegateHttpClientResponse& Callback);

	/** Cancels a queued or in-flight request: its callback receives the Cancelled error. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool Cancel(int32 RequestId);

	/** Sets the host from a URL ("https://host[:port]/..."); the path part is not used (set FHttpRequest.Path). */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	bool SetUrl(const FString& Url);

	/** Follows up to this many redirects (301, 302, 303, 307, 308) on the same host. 0 = deliver them as responses. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	int32 MaxRedirects = 0;

	/** Download progress of the current request: bytes received and Content-Length (-1 if unknown). */
	UPROPERTY(BlueprintCallable, BlueprintAssignable, Category = "IP|HTTP|Events")
	FDelegateHttpClientProgress OnProgress;


	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void Close();

private:
	friend struct IPNet::FHttpAccess;
	FIpHttpClientState State;
	FClientBindOptions bind_options;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
};
