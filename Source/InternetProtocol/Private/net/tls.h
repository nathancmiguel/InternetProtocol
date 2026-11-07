/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include <memory>
#include <string>

/*
 * TLS configuration
 * -----------------
 * One asio::ssl::context is built per UObject from FSecurityContextOpts and shared (read-only) by every
 * stream created from it. Defaults are secure:
 *  - TLS 1.2 minimum (SSLv2/3, TLS 1.0/1.1 and compression disabled), TLS 1.3 negotiated when available.
 *  - Peer verification with the engine root certificate store (SSL module) plus optional extra CAs.
 *  - Client streams send SNI and verify the certificate host name against the address passed to Connect
 *    (or FSecurityContextOpts::Host_Name_Verification when set).
 * Verify_Mode = None is still available as an explicit opt-out and logs a warning every time it is used.
 * All OpenSSL/Asio calls use error_code overloads; a configuration error makes connections fail with
 * EError::TlsSetupFailed instead of silently running with a broken context.
 */
namespace IPNet {

	struct FTlsConfig {
		std::shared_ptr<asio::ssl::context> Context;
		bool bServer = false;
		bool bVerifyPeer = true;
		bool bVerifyHostname = true;
		std::string HostnameOverride;
		/** Empty when the context was configured successfully. */
		FString Error;

		bool IsValid() const { return Context != nullptr && Error.IsEmpty(); }

		/** Applies SNI and host name verification to a client stream before its handshake. */
		bool ConfigureClientStream(asio::ssl::stream<asio::ip::tcp::socket>& Stream, const std::string& Host,
		                           asio::error_code& OutError) const;
	};

	/** Builds a TLS configuration. Never returns nullptr; check IsValid() / Error. Game Thread or any thread. */
	std::shared_ptr<const FTlsConfig> MakeTlsConfig(const FSecurityContextOpts& Opts, bool bServer);

	/** Called from module startup/shutdown to initialize the engine SSL library. */
	void InitializeTls();
	void ShutdownTls();
}
