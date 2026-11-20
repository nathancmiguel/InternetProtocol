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

TSet<UHttpRemote*> UHttpServer::Clients() {
	TSet<UHttpRemote*> Result;
	Result.Reserve(ClientSet.Num());
	for (const TObjectPtr<UHttpRemote>& Remote : ClientSet) {
		Result.Add(Remote.Get());
	}
	return Result;
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

void UHttpServer::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
	bUseTls = true;
}

bool UHttpServer::Open(const FServerBindOptions& BindOpts) {
	if (bUseTls && (!Tls || !Tls->IsValid())) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: a TLS server requires SetTlsOptions() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	return FFacadeAccess::OpenServer<UHttpServer>(this, BindOpts, [](UHttpServer* Self, asio::ip::tcp::socket&& Socket) {
		UHttpRemote* Remote = FHttpAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, Self->bUseTls);
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
	TArray<TObjectPtr<UHttpRemote>> Remotes = ClientSet.Array();
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

