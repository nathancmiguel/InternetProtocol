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
	all_cb.Add(Path, Callback);
}

void UHttpServer::Get(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	get_cb.Add(Path, Callback);
}

void UHttpServer::Post(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	post_cb.Add(Path, Callback);
}

void UHttpServer::Put(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	put_cb.Add(Path, Callback);
}

void UHttpServer::Del(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	del_cb.Add(Path, Callback);
}

void UHttpServer::Head(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	head_cb.Add(Path, Callback);
}

void UHttpServer::Options(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	options_cb.Add(Path, Callback);
}

void UHttpServer::Patch(const FString& Path, const FDelegateHttpServerRequest& Callback) {
	patch_cb.Add(Path, Callback);
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
	ClientSet.Empty();
	for (UHttpRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

void UHttpServer::Route(UHttpRemote* Remote, const FHttpRequest& Request) {
	FHttpAccess::PrepareResponse(Remote, Request);

	if (Request.Method == ERequestMethod::UNKNOWN) {
		Remote->Headers.Status_Code = 405;
		Remote->Headers.Status_Message = TEXT("Method Not Allowed");
		Remote->Headers.Headers.Add(TEXT("Allow"), TEXT("DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT"));
		Remote->Headers.Body = TEXT("Method not supported.");
		Remote->Write(FDelegateHttpRemoteMessageSent());
		return;
	}

	TMap<FString, FDelegateHttpServerRequest>* MethodRoutes = nullptr;
	switch (Request.Method) {
	case ERequestMethod::DEL: MethodRoutes = &del_cb; break;
	case ERequestMethod::GET: MethodRoutes = &get_cb; break;
	case ERequestMethod::HEAD: MethodRoutes = &head_cb; break;
	case ERequestMethod::OPTIONS: MethodRoutes = &options_cb; break;
	case ERequestMethod::POST: MethodRoutes = &post_cb; break;
	case ERequestMethod::PUT: MethodRoutes = &put_cb; break;
	case ERequestMethod::PATCH: MethodRoutes = &patch_cb; break;
	default: break;
	}

	// Copy the delegates: a handler may register routes or close the server while we iterate.
	TArray<FDelegateHttpServerRequest> Handlers;
	if (const FDelegateHttpServerRequest* Any = all_cb.Find(Request.Path)) {
		Handlers.Add(*Any);
	}
	if (MethodRoutes) {
		if (const FDelegateHttpServerRequest* Specific = MethodRoutes->Find(Request.Path)) {
			Handlers.Add(*Specific);
		}
	}

	bool bHandled = false;
	for (const FDelegateHttpServerRequest& Handler : Handlers) {
		if (Handler.IsBound()) {
			bHandled = true;
			Handler.ExecuteIfBound(Request, Remote);
		}
	}

	if (!bHandled && IsValid(Remote)) {
		Remote->Headers.Status_Code = 404;
		Remote->Headers.Status_Message = TEXT("Not Found");
		Remote->Headers.Body = TEXT("Not Found");
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
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
	all_cb.Add(Path, Callback);
}

void UHttpServerSsl::Get(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	get_cb.Add(Path, Callback);
}

void UHttpServerSsl::Post(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	post_cb.Add(Path, Callback);
}

void UHttpServerSsl::Put(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	put_cb.Add(Path, Callback);
}

void UHttpServerSsl::Del(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	del_cb.Add(Path, Callback);
}

void UHttpServerSsl::Head(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	head_cb.Add(Path, Callback);
}

void UHttpServerSsl::Options(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	options_cb.Add(Path, Callback);
}

void UHttpServerSsl::Patch(const FString& Path, const FDelegateHttpServerRequestSsl& Callback) {
	patch_cb.Add(Path, Callback);
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
	ClientSet.Empty();
	for (UHttpRemoteSsl* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

void UHttpServerSsl::Route(UHttpRemoteSsl* Remote, const FHttpRequest& Request) {
	FHttpAccess::PrepareResponse(Remote, Request);

	if (Request.Method == ERequestMethod::UNKNOWN) {
		Remote->Headers.Status_Code = 405;
		Remote->Headers.Status_Message = TEXT("Method Not Allowed");
		Remote->Headers.Headers.Add(TEXT("Allow"), TEXT("DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT"));
		Remote->Headers.Body = TEXT("Method not supported.");
		Remote->Write(FDelegateHttpRemoteMessageSent());
		return;
	}

	TMap<FString, FDelegateHttpServerRequestSsl>* MethodRoutes = nullptr;
	switch (Request.Method) {
	case ERequestMethod::DEL: MethodRoutes = &del_cb; break;
	case ERequestMethod::GET: MethodRoutes = &get_cb; break;
	case ERequestMethod::HEAD: MethodRoutes = &head_cb; break;
	case ERequestMethod::OPTIONS: MethodRoutes = &options_cb; break;
	case ERequestMethod::POST: MethodRoutes = &post_cb; break;
	case ERequestMethod::PUT: MethodRoutes = &put_cb; break;
	case ERequestMethod::PATCH: MethodRoutes = &patch_cb; break;
	default: break;
	}

	// Copy the delegates: a handler may register routes or close the server while we iterate.
	TArray<FDelegateHttpServerRequestSsl> Handlers;
	if (const FDelegateHttpServerRequestSsl* Any = all_cb.Find(Request.Path)) {
		Handlers.Add(*Any);
	}
	if (MethodRoutes) {
		if (const FDelegateHttpServerRequestSsl* Specific = MethodRoutes->Find(Request.Path)) {
			Handlers.Add(*Specific);
		}
	}

	bool bHandled = false;
	for (const FDelegateHttpServerRequestSsl& Handler : Handlers) {
		if (Handler.IsBound()) {
			bHandled = true;
			Handler.ExecuteIfBound(Request, Remote);
		}
	}

	if (!bHandled && IsValid(Remote)) {
		Remote->Headers.Status_Code = 404;
		Remote->Headers.Status_Message = TEXT("Not Found");
		Remote->Headers.Body = TEXT("Not Found");
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
}
