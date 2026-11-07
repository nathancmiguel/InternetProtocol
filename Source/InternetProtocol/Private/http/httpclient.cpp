/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "http/httpclient.h"
#include "net/httpfacade.h"

using IPNet::FHttpAccess;

// ---------------------------------------------------------------------------------------------------------
// UHttpClient

void UHttpClient::BeginDestroy() {
	FHttpAccess::Release(State);
	Super::BeginDestroy();
}

void UHttpClient::AddToRoot() {
	Super::AddToRoot();
}

void UHttpClient::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UHttpClient::IsRooted() {
	return Super::IsRooted();
}

bool UHttpClient::IsOpen() {
	return State.Net.bOpen;
}

void UHttpClient::SetHost(const FClientBindOptions& BindOpts) {
	if (BindOpts.Address != bind_options.Address || BindOpts.Port != bind_options.Port || BindOpts.Protocol != bind_options.Protocol) {
		// A keep-alive connection to the previous host must not be reused for the new one.
		FHttpAccess::Detach(State);
	}
	bind_options = BindOpts;
}

void UHttpClient::Request(const FHttpRequest& Req, const FDelegateHttpClientResponse& Callback) {
	FHttpAccess::Request(this, Req, Callback, nullptr, false);
}

void UHttpClient::Close() {
	FHttpAccess::Close(this);
}

// ---------------------------------------------------------------------------------------------------------

void UHttpClientSsl::Construct(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
}

// ---------------------------------------------------------------------------------------------------------
// UHttpClientSsl

void UHttpClientSsl::BeginDestroy() {
	FHttpAccess::Release(State);
	Super::BeginDestroy();
}

void UHttpClientSsl::AddToRoot() {
	Super::AddToRoot();
}

void UHttpClientSsl::RemoveFromRoot() {
	Super::RemoveFromRoot();
}

bool UHttpClientSsl::IsRooted() {
	return Super::IsRooted();
}

bool UHttpClientSsl::IsOpen() {
	return State.Net.bOpen;
}

void UHttpClientSsl::SetHost(const FClientBindOptions& BindOpts) {
	if (BindOpts.Address != bind_options.Address || BindOpts.Port != bind_options.Port || BindOpts.Protocol != bind_options.Protocol) {
		// A keep-alive connection to the previous host must not be reused for the new one.
		FHttpAccess::Detach(State);
	}
	bind_options = BindOpts;
}

void UHttpClientSsl::Request(const FHttpRequest& Req, const FDelegateHttpClientResponse& Callback) {
	if (!Tls) {
		Tls = IPNet::MakeTlsConfig(FSecurityContextOpts(), false);
	}
	FHttpAccess::Request(this, Req, Callback, Tls, true);
}

void UHttpClientSsl::Close() {
	FHttpAccess::Close(this);
}
