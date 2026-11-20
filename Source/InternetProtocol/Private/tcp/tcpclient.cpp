/**
 * Copyright © 2025 Nathan Miguel
 */

#include "tcp/tcpclient.h"
#include "net/facade.h"

using IPNet::FFacadeAccess;

// ---------------------------------------------------------------------------------------------------------
// UTCPClient

void UTCPClient::BeginDestroy() {
	FFacadeAccess::ReleaseStream(Net);
	Super::BeginDestroy();
}

void UTCPClient::AddToRoot() {
	Super::AddToRoot();
}

void UTCPClient::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UTCPClient::IsRooted() {
	return Super::IsRooted();
}

bool UTCPClient::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UTCPClient::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UTCPClient::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UTCPClient::GetErrorCode() {
	return Net.LastError;
}

bool UTCPClient::Write(const FString& Message, const FDelegateTcpClientMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::ToUtf8(Message), Callback);
}

bool UTCPClient::WriteBuffer(const TArray<uint8>& Buffer, const FDelegateTcpClientMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), Callback);
}

void UTCPClient::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
	bUseTls = true;
}

bool UTCPClient::Connect(const FClientBindOptions& BindOpts) {
	if (bUseTls && !Tls) {
		// TLS without SetTlsOptions: the secure defaults (system roots, host name verification).
		Tls = IPNet::GetDefaultClientTlsConfig();
	}
	return FFacadeAccess::ConnectTcp(this, BindOpts, Tls, bUseTls);
}

void UTCPClient::Close() {
	FFacadeAccess::CloseStream(Net);
}

FConnectionStats UTCPClient::GetStats() {
	return FFacadeAccess::StreamStats(Net);
}

