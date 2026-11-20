/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "subsystem/ipsubsystem.h"
#include "utils/utils.h"

UUDPServer* UInternetProtocolSubsystem::CreateUDPServer() {
	UUDPServer* server = NewObject<UUDPServer>(this);
	return Track(server);
}

UUDPClient* UInternetProtocolSubsystem::CreateUDPClient() {
	UUDPClient* client = NewObject<UUDPClient>(this);
	return Track(client);
}

UTCPServer* UInternetProtocolSubsystem::CreateTCPServer() {
	UTCPServer* server = NewObject<UTCPServer>(this);
	return Track(server);
}

UTCPServerSsl* UInternetProtocolSubsystem::CreateTCPServerSsl(const FSecurityContextOpts &SecOpts) {
	UTCPServerSsl* server = NewObject<UTCPServerSsl>(this);
	server->Construct(SecOpts);
	return Track(server);
}

UTCPClient* UInternetProtocolSubsystem::CreateTCPClient() {
	UTCPClient* client = NewObject<UTCPClient>(this);
	return Track(client);
}

UTCPClientSsl* UInternetProtocolSubsystem::CreateTCPClientSsl(const FSecurityContextOpts& SecOpts) {
	UTCPClientSsl* client = NewObject<UTCPClientSsl>(this);
	client->Construct(SecOpts);
	return Track(client);
}

UHttpServer* UInternetProtocolSubsystem::CreateHttpServer() {
	UHttpServer* server = NewObject<UHttpServer>(this);
	return Track(server);
}

UHttpServerSsl* UInternetProtocolSubsystem::CreateHttpServerSsl(const FSecurityContextOpts& SecOpts) {
	UHttpServerSsl* server = NewObject<UHttpServerSsl>(this);
	server->Construct(SecOpts);
	return Track(server);
}

UHttpClient* UInternetProtocolSubsystem::CreateHttpClient() {
	UHttpClient* client = NewObject<UHttpClient>(this);
	return Track(client);
}

UHttpClientSsl* UInternetProtocolSubsystem::CreateHttpClientSsl(const FSecurityContextOpts& SecOpts) {
	UHttpClientSsl* client = NewObject<UHttpClientSsl>(this);
	client->Construct(SecOpts);
	return Track(client);
}

UWSServer* UInternetProtocolSubsystem::CreateWSServer() {
	UWSServer* server = NewObject<UWSServer>(this);
	return Track(server);
}

UWSServerSsl* UInternetProtocolSubsystem::CreateWSServerSsl(const FSecurityContextOpts& SecOpts) {
	UWSServerSsl* server = NewObject<UWSServerSsl>(this);
	server->Construct(SecOpts);
	return Track(server);
}

UWSClient* UInternetProtocolSubsystem::CreateWSClient() {
	UWSClient* client = NewObject<UWSClient>(this);
	return Track(client);
}

UWSClientSsl* UInternetProtocolSubsystem::CreateWSClientSsl(const FSecurityContextOpts& SecOpts) {
	UWSClientSsl* client = NewObject<UWSClientSsl>(this);
	client->Construct(SecOpts);
	return Track(client);
}

UJavaScriptObjectNotation* UInternetProtocolSubsystem::CreateJson() {
	UJavaScriptObjectNotation* json = NewObject<UJavaScriptObjectNotation>(this);
	return Track(json);
}

URTMPServer* UInternetProtocolSubsystem::CreateRTMPServer() {
	return Track(NewObject<URTMPServer>(this));
}

void UInternetProtocolSubsystem::Deinitialize() {
	for (const TWeakObjectPtr<UObject>& Weak : Created) {
		UObject* Object = Weak.Get();
		if (!Object) {
			continue;
		}
		if (UWSClient* WsClient = Cast<UWSClient>(Object)) { WsClient->Close(1001, TEXT("Going Away")); }
		else if (UWSClientSsl* WssClient = Cast<UWSClientSsl>(Object)) { WssClient->Close(1001, TEXT("Going Away")); }
		else if (UWSServer* WsServer = Cast<UWSServer>(Object)) { WsServer->Close(); }
		else if (UWSServerSsl* WssServer = Cast<UWSServerSsl>(Object)) { WssServer->Close(); }
		else if (UTCPClient* TcpClient = Cast<UTCPClient>(Object)) { TcpClient->Close(); }
		else if (UTCPClientSsl* TcpClientSsl = Cast<UTCPClientSsl>(Object)) { TcpClientSsl->Close(); }
		else if (UTCPServer* TcpServer = Cast<UTCPServer>(Object)) { TcpServer->Close(); }
		else if (UTCPServerSsl* TcpServerSsl = Cast<UTCPServerSsl>(Object)) { TcpServerSsl->Close(); }
		else if (UHttpClient* HttpClient = Cast<UHttpClient>(Object)) { HttpClient->Close(); }
		else if (UHttpClientSsl* HttpClientSsl = Cast<UHttpClientSsl>(Object)) { HttpClientSsl->Close(); }
		else if (UHttpServer* HttpServer = Cast<UHttpServer>(Object)) { HttpServer->Close(); }
		else if (UHttpServerSsl* HttpServerSsl = Cast<UHttpServerSsl>(Object)) { HttpServerSsl->Close(); }
		else if (UUDPClient* UdpClient = Cast<UUDPClient>(Object)) { UdpClient->Close(); }
		else if (UUDPServer* UdpServer = Cast<UUDPServer>(Object)) { UdpServer->Close(); }
		else if (URTMPServer* Rtmp = Cast<URTMPServer>(Object)) { Rtmp->Close(); }
	}
	Created.Empty();
	Super::Deinitialize();
}
