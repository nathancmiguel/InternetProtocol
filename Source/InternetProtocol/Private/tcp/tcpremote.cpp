/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "tcp/tcpremote.h"
#include "net/facade.h"

using IPNet::FFacadeAccess;

// ---------------------------------------------------------------------------------------------------------
// UTCPRemote

void UTCPRemote::BeginDestroy() {
	FFacadeAccess::ReleaseStream(Net);
	OnClosedNative = nullptr;
	Super::BeginDestroy();
}

bool UTCPRemote::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UTCPRemote::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UTCPRemote::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UTCPRemote::GetErrorCode() {
	return Net.LastError;
}

bool UTCPRemote::Write(const FString& Message, const FDelegateTcpRemoteMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::ToUtf8(Message), Callback);
}

bool UTCPRemote::WriteBuffer(const TArray<uint8>& Buffer, const FDelegateTcpRemoteMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), Callback);
}

void UTCPRemote::Close() {
	FFacadeAccess::CloseStream(Net);
}

// ---------------------------------------------------------------------------------------------------------
// UTCPRemoteSsl

void UTCPRemoteSsl::BeginDestroy() {
	FFacadeAccess::ReleaseStream(Net);
	OnClosedNative = nullptr;
	Super::BeginDestroy();
}

bool UTCPRemoteSsl::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UTCPRemoteSsl::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UTCPRemoteSsl::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UTCPRemoteSsl::GetErrorCode() {
	return Net.LastError;
}

bool UTCPRemoteSsl::Write(const FString& Message, const FDelegateTcpRemoteMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::ToUtf8(Message), Callback);
}

bool UTCPRemoteSsl::WriteBuffer(const TArray<uint8>& Buffer, const FDelegateTcpRemoteMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), Callback);
}

void UTCPRemoteSsl::Close() {
	FFacadeAccess::CloseStream(Net);
}
