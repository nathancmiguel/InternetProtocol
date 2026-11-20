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

void UHttpClient::SetTlsOptions(const FSecurityContextOpts& SecOpts) {
	Tls = IPNet::MakeTlsConfig(SecOpts, false);
	bUseTls = true;
}

void UHttpClient::SetHost(const FClientBindOptions& BindOpts) {
	if (BindOpts.Address != bind_options.Address || BindOpts.Port != bind_options.Port || BindOpts.Protocol != bind_options.Protocol) {
		// A keep-alive connection to the previous host must not be reused, and its requests are cancelled.
		FHttpAccess::ChangeHost(this);
	}
	bind_options = BindOpts;
}

int32 UHttpClient::Request(const FHttpRequest& Req, const FDelegateHttpClientResponse& Callback) {
	if (bUseTls && !Tls) {
		Tls = IPNet::GetDefaultClientTlsConfig();
	}
	return FHttpAccess::Request(this, Req, Callback, Tls, bUseTls);
}

void UHttpClient::Close() {
	FHttpAccess::Close(this);
}

// ---------------------------------------------------------------------------------------------------------

bool UHttpClient::Cancel(int32 RequestId) {
	return FHttpAccess::Cancel(this, RequestId);
}

bool UHttpClient::SetUrl(const FString& Url) {
	IPNet::FParsedHttpUrl Parsed;
	if (!IPNet::ParseHttpUrl(Url, Parsed) || (Parsed.Scheme != TEXT("http") && Parsed.Scheme != TEXT("https"))) {
		UE_LOG(LogAsio, Warning, TEXT("HTTP SetUrl: expected a http:// or https:// URL."));
		return false;
	}
	const bool bHttps = Parsed.Scheme == TEXT("https");
	if (bHttps != bUseTls) {
		// The scheme decides; the current connection (other protocol) must not be reused.
		FHttpAccess::ChangeHost(this);
		bUseTls = bHttps;
	}
	FClientBindOptions Options = bind_options;
	Options.Address = Parsed.Host;
	Options.Port = FString::FromInt(Parsed.Port);
	SetHost(Options);
	return true;
}

