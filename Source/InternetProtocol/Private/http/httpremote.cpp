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

// ---------------------------------------------------------------------------------------------------------
// UHttpRemoteSsl

void UHttpRemoteSsl::BeginDestroy() {
	FHttpAccess::ReleaseRemote(this);
	Super::BeginDestroy();
}

bool UHttpRemoteSsl::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UHttpRemoteSsl::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UHttpRemoteSsl::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UHttpRemoteSsl::GetErrorCode() {
	return Net.LastError;
}

bool UHttpRemoteSsl::Write(const FDelegateHttpRemoteMessageSent& Callback) {
	return FHttpAccess::WriteResponse(this, Callback);
}

void UHttpRemoteSsl::Close() {
	FHttpAccess::CloseRemote(this);
}
