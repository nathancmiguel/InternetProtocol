/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "http/httpserver.h"
#include "net/httpfacade.h"

using IPNet::FFacadeAccess;
using IPNet::FHttpAccess;

// ---------------------------------------------------------------------------------------------------------
// UHttpServer

void UHttpServer::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UHttpServer::AddToRoot() {
	Super::AddToRoot();
}

void UHttpServer::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UHttpServer::IsRooted() {
	return Super::IsRooted();
}

bool UHttpServer::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UHttpServer::LocalEndpoint() {
	return Server.Local;
}

TSet<UHttpRemote*>& UHttpServer::Clients() {
	return ClientSet;
}

FErrorCode UHttpServer::GetErrorCode() {
	return Server.LastError;
}

void UHttpServer::All(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(all_cb, Path, Callback);
}

void UHttpServer::Get(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(get_cb, Path, Callback);
}

void UHttpServer::Post(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(post_cb, Path, Callback);
}

void UHttpServer::Put(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(put_cb, Path, Callback);
}

void UHttpServer::Del(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(del_cb, Path, Callback);
}

void UHttpServer::Head(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(head_cb, Path, Callback);
}

void UHttpServer::Options(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(options_cb, Path, Callback);
}

void UHttpServer::Patch(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	FHttpAccess::AddRoute(patch_cb, Path, Callback);
}

bool UHttpServer::Open(const FServerBindOptions& BindOpts) {
	return FFacadeAccess::OpenServer<UHttpServer>(this, BindOpts, [](UHttpServer* Self, asio::ip::tcp::socket&& Socket) {
		UHttpRemote* Remote = FHttpAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), nullptr, false);
		if (!Remote) {
			return;
		}
		TWeakObjectPtr<UHttpServer> WeakServer(Self);
		TWeakObjectPtr<UHttpRemote> WeakRemote(Remote);
		FHttpAccess::SetRequestHandler(Remote, [WeakServer, WeakRemote](const FHttpRequest& Request) {
			UHttpServer* ServerPtr = WeakServer.Get();
			UHttpRemote* RemotePtr = WeakRemote.Get();
			if (ServerPtr && RemotePtr) {
				ServerPtr->Route(RemotePtr, Request);
			}
		});
		FHttpAccess::StartRemote(Remote);
	});
}

void UHttpServer::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<UHttpRemote*> Remotes = ClientSet.Array();
	// Remotes stay referenced until their own OnClose (they remove themselves then).
	for (UHttpRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

void UHttpServer::Route(UHttpRemote* Remote, const FHttpRequest& Request) {
	FHttpAccess::PrepareResponse(Remote, Request);
	FHttpAccess::RouteRequest<FDelegateHttpServerRequest>(Remote, Request, ResponseTimeoutSeconds, all_cb, get_cb, post_cb, put_cb, del_cb, head_cb, options_cb, patch_cb);
}

// ---------------------------------------------------------------------------------------------------------

void UHttpServerSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
}

// ---------------------------------------------------------------------------------------------------------
// UHttpServerSsl

void UHttpServerSsl::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UHttpServerSsl::AddToRoot() {
	Super::AddToRoot();
}

void UHttpServerSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UHttpServerSsl::IsRooted() {
	return Super::IsRooted();
}

bool UHttpServerSsl::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UHttpServerSsl::LocalEndpoint() {
	return Server.Local;
}

TSet<UHttpRemoteSsl*>& UHttpServerSsl::Clients() {
	return ClientSet;
}

FErrorCode UHttpServerSsl::GetErrorCode() {
	return Server.LastError;
}

void UHttpServerSsl::All(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(all_cb, Path, Callback);
}

void UHttpServerSsl::Get(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(get_cb, Path, Callback);
}

void UHttpServerSsl::Post(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(post_cb, Path, Callback);
}

void UHttpServerSsl::Put(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(put_cb, Path, Callback);
}

void UHttpServerSsl::Del(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(del_cb, Path, Callback);
}

void UHttpServerSsl::Head(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(head_cb, Path, Callback);
}

void UHttpServerSsl::Options(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(options_cb, Path, Callback);
}

void UHttpServerSsl::Patch(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	FHttpAccess::AddRoute(patch_cb, Path, Callback);
}

bool UHttpServerSsl::Open(const FServerBindOptions& BindOpts) {
	if (!Tls || !Tls->IsValid()) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: UHttpServerSsl requires Construct() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	return FFacadeAccess::OpenServer<UHttpServerSsl>(this, BindOpts, [](UHttpServerSsl* Self, asio::ip::tcp::socket&& Socket) {
		UHttpRemoteSsl* Remote = FHttpAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, true);
		if (!Remote) {
			return;
		}
		TWeakObjectPtr<UHttpServerSsl> WeakServer(Self);
		TWeakObjectPtr<UHttpRemoteSsl> WeakRemote(Remote);
		FHttpAccess::SetRequestHandler(Remote, [WeakServer, WeakRemote](const FHttpRequest& Request) {
			UHttpServerSsl* ServerPtr = WeakServer.Get();
			UHttpRemoteSsl* RemotePtr = WeakRemote.Get();
			if (ServerPtr && RemotePtr) {
				ServerPtr->Route(RemotePtr, Request);
			}
		});
		FHttpAccess::StartRemote(Remote);
	});
}

void UHttpServerSsl::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<UHttpRemoteSsl*> Remotes = ClientSet.Array();
	// Remotes stay referenced until their own OnClose (they remove themselves then).
	for (UHttpRemoteSsl* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

void UHttpServerSsl::Route(UHttpRemoteSsl* Remote, const FHttpRequest& Request) {
	FHttpAccess::PrepareResponse(Remote, Request);
	FHttpAccess::RouteRequest<FDelegateHttpServerRequestSsl>(Remote, Request, ResponseTimeoutSeconds, all_cb, get_cb, post_cb, put_cb, del_cb, head_cb, options_cb, patch_cb);
}
