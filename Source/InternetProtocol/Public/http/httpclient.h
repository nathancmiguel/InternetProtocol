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

	UFUNCTION(blueprintcallable, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void AddToRoot();

	UFUNCTION(blueprintcallable, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
	void RemoveFromRoot();

	UFUNCTION(blueprintcallable, BlueprintPure, Category = "IP|HTTP", meta = (DeprecatedFunction, DeprecationMessage = "Keep a reference in a variable instead: objects referenced by a Blueprint stay alive. Will be removed in a future version."))
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


	/** Encrypts the connection with TLS, verifying the server with these options. Applies to the next connection. */
	UFUNCTION(BlueprintCallable, Category = "IP|HTTP")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** TLS (with the default, verifying options unless SetTlsOptions was called). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|HTTP")
	bool bUseTls = false;

	/** Same as SetTlsOptions (name kept from the former Ssl classes). */
	void Construct(const FSecurityContextOpts& SecOpts) { SetTlsOptions(SecOpts); }

private:
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
	friend struct IPNet::FHttpAccess;
	FIpHttpClientState State;
	FClientBindOptions bind_options;
};

/** UHttpClient with TLS on (kept for existing Blueprints and code). New code: UHttpClient with SetTlsOptions / bUseTls. */
UCLASS(Blueprintable, BlueprintType, Category = "IP|HTTP")
class INTERNETPROTOCOL_API UHttpClientSsl : public UHttpClient
{
	GENERATED_BODY()
public:
	UHttpClientSsl() { bUseTls = true; }
};
