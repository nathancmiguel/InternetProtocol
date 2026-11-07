/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "websocket/wsclient.h"
#include "net/wsfacade.h"

using IPNet::FWsAccess;

// ---------------------------------------------------------------------------------------------------------
// UWSClient

void UWSClient::BeginDestroy() {
	FWsAccess::Release(this);
	Super::BeginDestroy();
}

void UWSClient::AddToRoot() {
	Super::AddToRoot();
}

void UWSClient::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UWSClient::IsRooted() {
	return Super::IsRooted();
}

bool UWSClient::IsOpen() {
	return FWsAccess::IsOpen(this);
}

FTcpEndpoint UWSClient::LocalEndpoint() {
	return State.Net.Local;
}

FTcpEndpoint UWSClient::RemoteEndpoint() {
	return State.Net.Remote;
}

FErrorCode UWSClient::GetErrorCode() {
	return State.Net.LastError;
}

bool UWSClient::Write(const FString& Message, const FDataframe& Dataframe, const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Write(this, Message, Dataframe, Callback);
}

bool UWSClient::WriteBuffer(const TArray<uint8>& Buffer, const FDataframe& Dataframe, const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::WriteBuffer(this, Buffer, Dataframe, Callback);
}

bool UWSClient::Ping(const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PING, Callback);
}

bool UWSClient::Pong(const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PONG, Callback);
}

bool UWSClient::Connect(const FClientBindOptions& BindOpts) {
	return FWsAccess::Connect(this, BindOpts, nullptr, false);
}

void UWSClient::End(int code, const FString& reason) {
	FWsAccess::End(this, code, reason);
}

void UWSClient::Close(int code, const FString& reason) {
	FWsAccess::Close(this, code, reason);
}

// ---------------------------------------------------------------------------------------------------------

void UWSClientSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
}

// ---------------------------------------------------------------------------------------------------------
// UWSClientSsl

void UWSClientSsl::BeginDestroy() {
	FWsAccess::Release(this);
	Super::BeginDestroy();
}

void UWSClientSsl::AddToRoot() {
	Super::AddToRoot();
}

void UWSClientSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UWSClientSsl::IsRooted() {
	return Super::IsRooted();
}

bool UWSClientSsl::IsOpen() {
	return FWsAccess::IsOpen(this);
}

FTcpEndpoint UWSClientSsl::LocalEndpoint() {
	return State.Net.Local;
}

FTcpEndpoint UWSClientSsl::RemoteEndpoint() {
	return State.Net.Remote;
}

FErrorCode UWSClientSsl::GetErrorCode() {
	return State.Net.LastError;
}

bool UWSClientSsl::Write(const FString& Message, const FDataframe& Dataframe, const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Write(this, Message, Dataframe, Callback);
}

bool UWSClientSsl::WriteBuffer(const TArray<uint8>& Buffer, const FDataframe& Dataframe, const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::WriteBuffer(this, Buffer, Dataframe, Callback);
}

bool UWSClientSsl::Ping(const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PING, Callback);
}

bool UWSClientSsl::Pong(const FDelegateWsClientMessageSent& Callback) {
	return FWsAccess::Control(this, EOpcode::PONG, Callback);
}

bool UWSClientSsl::Connect(const FClientBindOptions& BindOpts) {
	if (!Tls) {
		Tls = IPNet::MakeTlsConfig(FSecurityContextOpts(), false);
	}
	return FWsAccess::Connect(this, BindOpts, Tls, true);
}

void UWSClientSsl::End(int code, const FString& reason) {
	FWsAccess::End(this, code, reason);
}

void UWSClientSsl::Close(int code, const FString& reason) {
	FWsAccess::Close(this, code, reason);
}
