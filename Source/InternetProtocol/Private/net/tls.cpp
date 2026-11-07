/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/tls.h"
#include "Misc/Base64.h"
#include "Ssl.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Interfaces/ISslManager.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/ssl.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace IPNet {
	namespace {
		std::atomic<bool> GSslInitialized{false};

		/** Keeps the UTF-8 bytes alive for the duration of the OpenSSL call. */
		struct FUtf8Buffer {
			explicit FUtf8Buffer(const FString& Text) : Converter(*Text) {}
			asio::const_buffer Buffer() const {
				return asio::const_buffer(Converter.Get(), static_cast<size_t>(Converter.Length()));
			}
			FTCHARToUTF8 Converter;
		};

		/**
		 * ASN.1 (DER) data cannot be represented in an FString without loss, so it is only accepted
		 * base64-encoded. PEM text is passed through as UTF-8.
		 */
		bool ToKeyMaterial(const FString& Text, EFileFormat Format, std::vector<uint8>& Out) {
			Out.clear();
			if (Format == EFileFormat::pem) {
				const FTCHARToUTF8 Utf8(*Text);
				Out.assign(reinterpret_cast<const uint8*>(Utf8.Get()), reinterpret_cast<const uint8*>(Utf8.Get()) + Utf8.Length());
				return true;
			}
			TArray<uint8> Decoded;
			if (!FBase64::Decode(Text.TrimStartAndEnd(), Decoded)) {
				return false;
			}
			Out.assign(Decoded.GetData(), Decoded.GetData() + Decoded.Num());
			return true;
		}

		FString ErrorText(const TCHAR* What, const asio::error_code& Ec) {
			return FString::Printf(TEXT("%s: %hs"), What, Ec.message().c_str());
		}

		bool IsIpLiteral(const std::string& Host) {
			asio::error_code Ec;
			asio::ip::make_address(Host, Ec);
			return !Ec;
		}
	}

	void InitializeTls() {
		if (!GSslInitialized.exchange(true)) {
			FSslModule::Get().GetSslManager().InitializeSsl();
		}
	}

	void ShutdownTls() {
		if (GSslInitialized.exchange(false)) {
			if (FModuleManager::Get().IsModuleLoaded(TEXT("SSL"))) {
				FSslModule::Get().GetSslManager().ShutdownSsl();
			}
		}
	}

	bool FTlsConfig::ConfigureClientStream(asio::ssl::stream<asio::ip::tcp::socket>& Stream, const std::string& Host,
	                                       asio::error_code& OutError) const {
		OutError.clear();
		const std::string& Name = HostnameOverride.empty() ? Host : HostnameOverride;

		// SNI is only defined for DNS names (RFC 6066 section 3).
		if (!Name.empty() && !IsIpLiteral(Name)) {
			if (SSL_set_tlsext_host_name(Stream.native_handle(), Name.c_str()) != 1) {
				OutError = asio::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
				return false;
			}
		}

		if (bVerifyPeer && bVerifyHostname) {
			if (Name.empty()) {
				OutError = asio::error::invalid_argument;
				return false;
			}
			Stream.set_verify_callback(asio::ssl::host_name_verification(Name), OutError);
			if (OutError) {
				return false;
			}
		}
		return true;
	}

	std::shared_ptr<const FTlsConfig> MakeTlsConfig(const FSecurityContextOpts& Opts, bool bServer) {
		std::shared_ptr<FTlsConfig> Config = std::make_shared<FTlsConfig>();
		Config->bServer = bServer;
		Config->Context = std::make_shared<asio::ssl::context>(bServer ? asio::ssl::context::tls_server
		                                                               : asio::ssl::context::tls_client);
		asio::ssl::context& Ctx = *Config->Context;
		asio::error_code Ec;

		auto Fail = [&Config](const FString& Message) {
			Config->Error = Message;
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol TLS: %s"), *Message);
			return std::shared_ptr<const FTlsConfig>(Config);
		};

		Ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
		                asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
		                asio::ssl::context::no_tlsv1_1 | asio::ssl::context::no_compression |
		                asio::ssl::context::single_dh_use, Ec);
		if (Ec) {
			return Fail(ErrorText(TEXT("set_options failed"), Ec));
		}
		if (SSL_CTX_set_min_proto_version(Ctx.native_handle(), TLS1_2_VERSION) != 1) {
			return Fail(TEXT("could not enforce TLS 1.2 as minimum protocol version"));
		}

		// Trust store.
		if (Opts.bUseSystemRootCertificates) {
			InitializeTls();
			ISslCertificateManager& Certificates = FSslModule::Get().GetCertificateManager();
			if (Certificates.HasCertificatesAvailable()) {
				Certificates.AddCertificatesToSslContext(Ctx.native_handle());
			} else {
				Ctx.set_default_verify_paths(Ec);
				if (Ec) {
					UE_LOG(LogAsio, Warning, TEXT("InternetProtocol TLS: no root certificates available (%hs)."), Ec.message().c_str());
					Ec.clear();
				}
			}
		}
		if (!Opts.CA_Cert.IsEmpty()) {
			const FUtf8Buffer Ca(Opts.CA_Cert);
			Ctx.add_certificate_authority(Ca.Buffer(), Ec);
			if (Ec) {
				return Fail(ErrorText(TEXT("invalid CA_Cert"), Ec));
			}
		}

		// Own identity.
		const asio::ssl::context::file_format Format = Opts.File_Format == EFileFormat::asn1
			                                               ? asio::ssl::context::asn1
			                                               : asio::ssl::context::pem;
		std::vector<uint8> Material;
		if (!Opts.Cert_Chain.IsEmpty()) {
			const FUtf8Buffer Chain(Opts.Cert_Chain);
			Ctx.use_certificate_chain(Chain.Buffer(), Ec);
			if (Ec) {
				return Fail(ErrorText(TEXT("invalid Cert_Chain"), Ec));
			}
		}
		if (!Opts.Cert.IsEmpty()) {
			if (!ToKeyMaterial(Opts.Cert, Opts.File_Format, Material)) {
				return Fail(TEXT("Cert is not valid base64 (required for asn1)"));
			}
			Ctx.use_certificate(asio::buffer(Material), Format, Ec);
			if (Ec) {
				return Fail(ErrorText(TEXT("invalid Cert"), Ec));
			}
		}
		if (!Opts.Private_Key.IsEmpty()) {
			if (!ToKeyMaterial(Opts.Private_Key, Opts.File_Format, Material)) {
				return Fail(TEXT("Private_Key is not valid base64 (required for asn1)"));
			}
			Ctx.use_private_key(asio::buffer(Material), Format, Ec);
			if (Ec) {
				return Fail(ErrorText(TEXT("invalid Private_Key"), Ec));
			}
		}
		if (!Opts.RSA_Private_Key.IsEmpty()) {
			if (!ToKeyMaterial(Opts.RSA_Private_Key, Opts.File_Format, Material)) {
				return Fail(TEXT("RSA_Private_Key is not valid base64 (required for asn1)"));
			}
			Ctx.use_rsa_private_key(asio::buffer(Material), Format, Ec);
			if (Ec) {
				return Fail(ErrorText(TEXT("invalid RSA_Private_Key"), Ec));
			}
		}
		std::fill(Material.begin(), Material.end(), 0);

		const bool bHasCert = !Opts.Cert.IsEmpty() || !Opts.Cert_Chain.IsEmpty();
		const bool bHasKey = !Opts.Private_Key.IsEmpty() || !Opts.RSA_Private_Key.IsEmpty();
		if (bServer && (!bHasCert || !bHasKey)) {
			return Fail(TEXT("a TLS server requires a certificate (Cert or Cert_Chain) and a private key"));
		}
		if (bHasCert && bHasKey && SSL_CTX_check_private_key(Ctx.native_handle()) != 1) {
			return Fail(TEXT("the private key does not match the certificate"));
		}

		// Verification.
		asio::ssl::verify_mode Mode = asio::ssl::verify_peer;
		switch (Opts.Verify_Mode) {
		case EVerifyMode::None:
			Mode = asio::ssl::verify_none;
			Config->bVerifyPeer = false;
			if (!bServer) {
				// On a server, None only means "do not request a client certificate", which is the usual setup.
				UE_LOG(LogAsio, Warning, TEXT("InternetProtocol TLS: server certificate verification is DISABLED (Verify_Mode = None). "
					"Connections are vulnerable to man-in-the-middle attacks; use only for local testing."));
			}
			break;
		case EVerifyMode::Verify_Fail_If_No_Peer_Cert:
			Mode = asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert;
			break;
		case EVerifyMode::Verify_Client_Once:
			Mode = asio::ssl::verify_peer | asio::ssl::verify_client_once;
			break;
		case EVerifyMode::Verify_Peer:
		default:
			Mode = asio::ssl::verify_peer;
			break;
		}
		Ctx.set_verify_mode(Mode, Ec);
		if (Ec) {
			return Fail(ErrorText(TEXT("set_verify_mode failed"), Ec));
		}

		Config->bVerifyHostname = Opts.bVerifyHostname;
		if (!Opts.Host_Name_Verification.IsEmpty()) {
			Config->HostnameOverride = TCHAR_TO_UTF8(*Opts.Host_Name_Verification);
			if (bServer && Config->bVerifyPeer) {
				// Server side: verify the client certificate against the configured name (previous behaviour).
				Ctx.set_verify_callback(asio::ssl::host_name_verification(Config->HostnameOverride), Ec);
				if (Ec) {
					return Fail(ErrorText(TEXT("set_verify_callback failed"), Ec));
				}
			}
		}
		return Config;
	}
}
