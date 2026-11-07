/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "websocket/wsremote.h"
#include "net/wsfacade.h"

using IPNet::FWsAccess;

// ---------------------------------------------------------------------------------------------------------
// UWSRemote

void UWSRemote::BeginDestroy() {
	FWsAccess::Release(this);
	OnClosedNative = nullptr;
	Super::BeginDestroy();
}

bool UWSRemote::IsOpen() {
	return FWsAccess::IsOpen(this);
}

FTcpEndpoint UWSRemote::LocalEndpoint() {
	return State.Net.Local;
}

FTcpEndpoint UWSRemote::RemoteEndpoint() {
	return State.Net.Remote;
}

FErrorCode UWSRemote::GetErrorCode() {
	return State.Net.LastError;
}

bool UWSRemote::Write(const FString& Message, const FDataframe& Dataframe, const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Write(this, Message, Dataframe, Callback);
}

bool UWSRemote::WriteBuffer(const TArray<uint8>& Buffer, const FDataframe& Dataframe, const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::WriteBuffer(this, Buffer, Dataframe, Callback);
}

bool UWSRemote::Ping(const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PING, Callback);
}

bool UWSRemote::Pong(const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PONG, Callback);
}

void UWSRemote::End(int code, const FString& reason) {
	FWsAccess::End(this, code, reason);
}

void UWSRemote::Close(int code, const FString& reason) {
	FWsAccess::Close(this, code, reason);
}

// ---------------------------------------------------------------------------------------------------------
// UWSRemoteSsl

void UWSRemoteSsl::BeginDestroy() {
	FWsAccess::Release(this);
	OnClosedNative = nullptr;
	Super::BeginDestroy();
}

bool UWSRemoteSsl::IsOpen() {
	return FWsAccess::IsOpen(this);
}

FTcpEndpoint UWSRemoteSsl::LocalEndpoint() {
	return State.Net.Local;
}

FTcpEndpoint UWSRemoteSsl::RemoteEndpoint() {
	return State.Net.Remote;
}

FErrorCode UWSRemoteSsl::GetErrorCode() {
	return State.Net.LastError;
}

bool UWSRemoteSsl::Write(const FString& Message, const FDataframe& Dataframe, const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Write(this, Message, Dataframe, Callback);
}

bool UWSRemoteSsl::WriteBuffer(const TArray<uint8>& Buffer, const FDataframe& Dataframe, const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::WriteBuffer(this, Buffer, Dataframe, Callback);
}

bool UWSRemoteSsl::Ping(const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PING, Callback);
}

bool UWSRemoteSsl::Pong(const FDelegateWsRemoteMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PONG, Callback);
}

void UWSRemoteSsl::End(int code, const FString& reason) {
	FWsAccess::End(this, code, reason);
}

void UWSRemoteSsl::Close(int code, const FString& reason) {
	FWsAccess::Close(this, code, reason);
}
