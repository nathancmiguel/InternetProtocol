/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "http/httpremote.h"
#include "net/httpfacade.h"

using IPNet::FHttpAccess;

// ---------------------------------------------------------------------------------------------------------
// UHttpRemote

void UHttpRemote::BeginDestroy() {
	FHttpAccess::ReleaseRemote(this);
	Super::BeginDestroy();
}

bool UHttpRemote::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UHttpRemote::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UHttpRemote::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UHttpRemote::GetErrorCode() {
	return Net.LastError;
}

bool UHttpRemote::Write(const FDelegateHttpRemoteMessageSent& Callback) {
	return FHttpAccess::WriteResponse(this, Callback);
}

void UHttpRemote::Close() {
	FHttpAccess::CloseRemote(this);
}

bool UHttpRemote::BeginStream() {
	return FHttpAccess::BeginStream(this);
}

bool UHttpRemote::WriteChunk(const FString& Data) {
	const IPNet::FBytes Utf8 = IPNet::ToUtf8(Data);
	return FHttpAccess::WriteChunk(this, Utf8.data(), Utf8.size());
}

bool UHttpRemote::WriteChunkBuffer(const TArray<uint8>& Data) {
	return FHttpAccess::WriteChunk(this, Data.GetData(), static_cast<size_t>(Data.Num()));
}

bool UHttpRemote::EndStream() {
	return FHttpAccess::EndStream(this);
}

