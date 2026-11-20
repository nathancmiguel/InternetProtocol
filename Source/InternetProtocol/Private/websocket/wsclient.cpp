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

void UWSClient::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
	bUseTls = true;
}

bool UWSClient::Connect(const FClientBindOptions& BindOpts) {
	if (bUseTls && !Tls) {
		Tls = IPNet::GetDefaultClientTlsConfig();
	}
	return FWsAccess::Connect(this, BindOpts, Tls, bUseTls);
}

void UWSClient::End(int code, const FString& reason) {
	FWsAccess::End(this, code, reason);
}

void UWSClient::Close(int code, const FString& reason) {
	FWsAccess::Close(this, code, reason);
}

// ---------------------------------------------------------------------------------------------------------

FString UWSClient::GetSubProtocol() {
	return State.SubProtocol;
}

float UWSClient::GetRoundTripTimeMs() {
	return State.RoundTripMs;
}

bool UWSClient::ConnectUrl(const FString& Url) {
	IPNet::FParsedHttpUrl Parsed;
	if (!IPNet::ParseHttpUrl(Url, Parsed) || (Parsed.Scheme != TEXT("ws") && Parsed.Scheme != TEXT("wss"))) {
		UE_LOG(LogAsio, Warning, TEXT("WebSocket ConnectUrl: expected a ws:// or wss:// URL."));
		return false;
	}
	// The scheme decides: wss:// turns TLS on, ws:// off (only if the connection actually starts).
	const bool bPreviousTls = bUseTls;
	const FString PreviousPath = Handshake.Path;
	bUseTls = Parsed.Scheme == TEXT("wss");
	Handshake.Path = Parsed.Target;
	FClientBindOptions Options;
	Options.Address = Parsed.Host;
	Options.Port = FString::FromInt(Parsed.Port);
	if (!Connect(Options)) {
		bUseTls = bPreviousTls;
		Handshake.Path = PreviousPath;
		return false;
	}
	return true;
}

FConnectionStats UWSClient::GetStats() {
	return FWsAccess::Stats(this);
}

