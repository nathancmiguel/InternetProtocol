/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "websocket/wsserver.h"
#include "net/wsfacade.h"

using IPNet::FFacadeAccess;
using IPNet::FWsAccess;

// ---------------------------------------------------------------------------------------------------------
// UWSServer

void UWSServer::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UWSServer::AddToRoot() {
	Super::AddToRoot();
}

void UWSServer::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UWSServer::IsRooted() {
	return Super::IsRooted();
}

bool UWSServer::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UWSServer::LocalEndpoint() {
	return Server.Local;
}

TSet<UWSRemote*> UWSServer::Clients() {
	TSet<UWSRemote*> Result;
	Result.Reserve(ClientSet.Num());
	for (const TObjectPtr<UWSRemote>& Remote : ClientSet) {
		Result.Add(Remote.Get());
	}
	return Result;
}

FErrorCode UWSServer::GetErrorCode() {
	return Server.LastError;
}

void UWSServer::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
	bUseTls = true;
}

bool UWSServer::Open(const FServerBindOptions& BindOpts) {
	if (bUseTls && (!Tls || !Tls->IsValid())) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: a TLS server requires SetTlsOptions() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	const bool bOpened = FFacadeAccess::OpenServer<UWSServer>(this, BindOpts, [](UWSServer* Self, asio::ip::tcp::socket&& Socket) {
		UWSRemote* Remote = FWsAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, Self->bUseTls);
		if (!Remote) {
			return;
		}
		Self->OnClientAccepted.Broadcast(Remote);
		// The handshake starts only now, so handlers bound in OnClientAccepted receive OnConnected.
		FWsAccess::StartRemote(Remote);
	});
	if (bOpened) {
		OnListening.Broadcast();
	}
	return bOpened;
}

void UWSServer::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<TObjectPtr<UWSRemote>> Remotes = ClientSet.Array();
	// Remotes stay referenced until their own OnClose (they remove themselves then).
	for (UWSRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->End(1001, TEXT("Server shutdown"));
		}
	}
}

// ---------------------------------------------------------------------------------------------------------

