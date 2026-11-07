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

bool UTCPClient::Connect(const FClientBindOptions& BindOpts) {
	return FFacadeAccess::ConnectTcp(this, BindOpts, nullptr, false);
}

void UTCPClient::Close() {
	FFacadeAccess::CloseStream(Net);
}

// ---------------------------------------------------------------------------------------------------------
// UTCPClientSsl

void UTCPClientSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
}

void UTCPClientSsl::BeginDestroy() {
	FFacadeAccess::ReleaseStream(Net);
	Super::BeginDestroy();
}

void UTCPClientSsl::AddToRoot() {
	Super::AddToRoot();
}

void UTCPClientSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UTCPClientSsl::IsRooted() {
	return Super::IsRooted();
}

bool UTCPClientSsl::IsOpen() {
	return Net.bOpen;
}

FTcpEndpoint UTCPClientSsl::LocalEndpoint() {
	return Net.Local;
}

FTcpEndpoint UTCPClientSsl::RemoteEndpoint() {
	return Net.Remote;
}

FErrorCode UTCPClientSsl::GetErrorCode() {
	return Net.LastError;
}

bool UTCPClientSsl::Write(const FString& Message, const FDelegateTcpClientMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::ToUtf8(Message), Callback);
}

bool UTCPClientSsl::WriteBuffer(const TArray<uint8>& Buffer, const FDelegateTcpClientMessageSent& Callback) {
	return FFacadeAccess::SendBytes(this, IPNet::FBytes(Buffer.GetData(), Buffer.GetData() + Buffer.Num()), Callback);
}

bool UTCPClientSsl::Connect(const FClientBindOptions& BindOpts) {
	if (!Tls) {
		// Created without the subsystem: use the secure defaults.
		Tls = IPNet::MakeTlsConfig(FSecurityContextOpts(), false);
	}
	return FFacadeAccess::ConnectTcp(this, BindOpts, Tls, true);
}

void UTCPClientSsl::Close() {
	FFacadeAccess::CloseStream(Net);
}
