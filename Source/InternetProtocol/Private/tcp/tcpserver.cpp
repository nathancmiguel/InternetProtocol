/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "tcp/tcpserver.h"
#include "net/facade.h"

using IPNet::FFacadeAccess;

// ---------------------------------------------------------------------------------------------------------
// UTCPServer

void UTCPServer::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	// Remotes are owned by this server (Outer) and close themselves when they are destroyed.
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UTCPServer::AddToRoot() {
	Super::AddToRoot();
}

void UTCPServer::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UTCPServer::IsRooted() {
	return Super::IsRooted();
}

bool UTCPServer::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UTCPServer::LocalEndpoint() {
	return Server.Local;
}

const TSet<UTCPRemote*>& UTCPServer::Clients() {
	return ClientSet;
}

FErrorCode UTCPServer::GetErrorCode() {
	return Server.LastError;
}

bool UTCPServer::Open(const FServerBindOptions& BindOpts) {
	const bool bOpened = FFacadeAccess::OpenServer<UTCPServer>(this, BindOpts, [](UTCPServer* Self, asio::ip::tcp::socket&& Socket) {
		UTCPRemote* Remote = FFacadeAccess::AdoptTcpRemote(Self, Self->ClientSet, MoveTemp(Socket), nullptr, false);
		if (!Remote) {
			return;
		}
		Self->OnClientAccepted.Broadcast(Remote);
		// Reading (and the TLS handshake) only starts now, so handlers bound in OnClientAccepted see every event.
		FFacadeAccess::StartRemote(Remote);
	});
	if (bOpened) {
		OnListening.Broadcast();
	}
	return bOpened;
}

void UTCPServer::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<UTCPRemote*> Remotes = ClientSet.Array();
	ClientSet.Empty();
	for (UTCPRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

// ---------------------------------------------------------------------------------------------------------

void UTCPServerSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
}

// ---------------------------------------------------------------------------------------------------------
// UTCPServerSsl

void UTCPServerSsl::BeginDestroy() {
	FFacadeAccess::ReleaseServer(Server);
	// Remotes are owned by this server (Outer) and close themselves when they are destroyed.
	ClientSet.Empty();
	Super::BeginDestroy();
}

void UTCPServerSsl::AddToRoot() {
	Super::AddToRoot();
}

void UTCPServerSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UTCPServerSsl::IsRooted() {
	return Super::IsRooted();
}

bool UTCPServerSsl::IsOpen() {
	return Server.bOpen;
}

FTcpEndpoint UTCPServerSsl::LocalEndpoint() {
	return Server.Local;
}

const TSet<UTCPRemoteSsl*>& UTCPServerSsl::Clients() {
	return ClientSet;
}

FErrorCode UTCPServerSsl::GetErrorCode() {
	return Server.LastError;
}

bool UTCPServerSsl::Open(const FServerBindOptions& BindOpts) {
	if (!Tls || !Tls->IsValid()) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: UTCPServerSsl requires Construct() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	const bool bOpened = FFacadeAccess::OpenServer<UTCPServerSsl>(this, BindOpts, [](UTCPServerSsl* Self, asio::ip::tcp::socket&& Socket) {
		UTCPRemoteSsl* Remote = FFacadeAccess::AdoptTcpRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, true);
		if (!Remote) {
			return;
		}
		Self->OnClientAccepted.Broadcast(Remote);
		// Reading (and the TLS handshake) only starts now, so handlers bound in OnClientAccepted see every event.
		FFacadeAccess::StartRemote(Remote);
	});
	if (bOpened) {
		OnListening.Broadcast();
	}
	return bOpened;
}

void UTCPServerSsl::Close() {
	FFacadeAccess::CloseServer(this);
	TArray<UTCPRemoteSsl*> Remotes = ClientSet.Array();
	ClientSet.Empty();
	for (UTCPRemoteSsl* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}
