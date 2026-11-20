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

FConnectionStats UTCPRemote::GetStats() {
	return FFacadeAccess::StreamStats(Net);
}

