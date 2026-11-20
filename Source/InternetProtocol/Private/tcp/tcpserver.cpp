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

TSet<UTCPRemote*> UTCPServer::Clients() {
	TSet<UTCPRemote*> Result;
	Result.Reserve(ClientSet.Num());
	for (const TObjectPtr<UTCPRemote>& Remote : ClientSet) {
		Result.Add(Remote.Get());
	}
	return Result;
}

FErrorCode UTCPServer::GetErrorCode() {
	return Server.LastError;
}

void UTCPServer::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, true);
	bUseTls = true;
}

bool UTCPServer::Open(const FServerBindOptions& BindOpts) {
	if (bUseTls && (!Tls || !Tls->IsValid())) {
		if (!Tls) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: a TLS server requires SetTlsOptions() with a certificate and key."));
		}
		Server.LastError = FErrorCode(IPNet::MakeError(IPNet::EError::TlsSetupFailed));
		OnError.Broadcast(Server.LastError);
		return false;
	}
	const bool bOpened = FFacadeAccess::OpenServer<UTCPServer>(this, BindOpts, [](UTCPServer* Self, asio::ip::tcp::socket&& Socket) {
		UTCPRemote* Remote = FFacadeAccess::AdoptTcpRemote(Self, Self->ClientSet, MoveTemp(Socket), Self->Tls, Self->bUseTls);
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
	TArray<TObjectPtr<UTCPRemote>> Remotes = ClientSet.Array();
	// Remotes stay referenced until their own OnClose (they remove themselves then).
	for (UTCPRemote* Remote : Remotes) {
		if (IsValid(Remote)) {
			Remote->Close();
		}
	}
}

// ---------------------------------------------------------------------------------------------------------

