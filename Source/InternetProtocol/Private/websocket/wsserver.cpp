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

const TSet<UWSRemote*>& UWSServer::Clients() {
	return ClientSet;
}

FErrorCode UWSServer::GetErrorCode() {
	return Server.LastError;
}

bool UWSServer::Open(const FServerBindOptions& BindOpts) {
	const bool bOpened = FFacadeAccess::OpenServer<UWSServer>(this, BindOpts, [](UWSServer* Self, asio::ip::tcp::socket&& Socket) {
		UWSRemote* Remote = FWsAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), nullptr, false);
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
	TArray<UWSRemote*> Remotes = ClientSet.Array();
	ClientSet.Empty();
	for (UWSRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->End(1001, TEXT("Server shutdown"));
		}
	}
}

// ---------------------------------------------------------------------------------------------------------

void UWSServerSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
}

// ---------------------------------------------------------------------------------------------------------
// UWSServerSsl

void UWSServerSsl::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UWSServerSsl::AddToRoot() {
	Super::AddToRoot();
}

void UWSServerSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UWSServerSsl::IsRooted() {
	return Super::IsRooted();
}

bool UWSServerSsl::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UWSServerSsl::LocalEndpoint() {
	return Server.Local;
}

const TSet<UWSRemoteSsl*>& UWSServerSsl::Clients() {
	return ClientSet;
}

FErrorCode UWSServerSsl::GetErrorCode() {
	return Server.LastError;
}

bool UWSServerSsl::Open(const FServerBindOptions& BindOpts) {
	if (!Tls || !Tls->IsValid()) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: UWSServerSsl requires Construct() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	const bool bOpened = FFacadeAccess::OpenServer<UWSServerSsl>(this, BindOpts, [](UWSServerSsl* Self, asio::ip::tcp::socket&& Socket) {
		UWSRemoteSsl* Remote = FWsAccess::AdoptRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, true);
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

void UWSServerSsl::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<UWSRemoteSsl*> Remotes = ClientSet.Array();
	ClientSet.Empty();
	for (UWSRemoteSsl* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->End(1001, TEXT("Server shutdown"));
		}
	}
}
