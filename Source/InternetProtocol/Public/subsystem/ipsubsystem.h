/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "udp/udpserver.h"
#include "udp/udpclient.h"
#include "tcp/tcpserver.h"
#include "tcp/tcpclient.h"
#include "http/httpclient.h"
#include "http/httpserver.h"
#include "websocket/wsserver.h"
#include "websocket/wsclient.h"
#include "json/json.h"
#include "rtmp/rtmpserver.h"
#include "Subsystems/WorldSubsystem.h"

#include "ipsubsystem.generated.h"

/**
 * 
 */

UCLASS()
class INTERNETPROTOCOL_API UInternetProtocolSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UUDPServer* CreateUDPServer();
	
	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UUDPClient* CreateUDPClient();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UTCPServer* CreateTCPServer();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UTCPServerSsl* CreateTCPServerSsl(const FSecurityContextOpts &SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UTCPClient* CreateTCPClient();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UTCPClientSsl* CreateTCPClientSsl(const FSecurityContextOpts &SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UHttpServer* CreateHttpServer();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UHttpServerSsl* CreateHttpServerSsl(const FSecurityContextOpts& SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UHttpClient* CreateHttpClient();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UHttpClientSsl* CreateHttpClientSsl(const FSecurityContextOpts& SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UWSServer* CreateWSServer();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UWSServerSsl* CreateWSServerSsl(const FSecurityContextOpts& SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UWSClient* CreateWSClient();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UWSClientSsl* CreateWSClientSsl(const FSecurityContextOpts& SecOpts);

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	UJavaScriptObjectNotation* CreateJson();

	UFUNCTION(BlueprintCallable, Category="IP|Subsystem")
	URTMPServer* CreateRTMPServer();

	/** Closes every connection / server created by this subsystem (their ports are freed when the world ends, e.g. at the end of PIE). */
	virtual void Deinitialize() override;

private:
	template <typename T>
	T* Track(T* Object) {
		Created.Add(Object);
		return Object;
	}

	/** Objects created here; weak: the caller decides how long they live. */
	TArray<TWeakObjectPtr<UObject>> Created;
};
