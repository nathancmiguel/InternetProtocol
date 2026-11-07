/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

// Must come first: THIRD_PARTY_INCLUDES_START and PLATFORM_* are defined by the engine headers.
#include "CoreMinimal.h"

/*
 * Asio configuration.
 * ASIO_STANDALONE / ASIO_NO_EXCEPTIONS / ASIO_NO_DEPRECATED are defined by InternetProtocol.Build.cs as
 * PublicDefinitions so that every translation unit (and every dependent module) sees the exact same Asio
 * configuration. The guards below only exist for tools that parse this header outside of UBT.
 */
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE 1
#endif
#ifndef ASIO_NO_DEPRECATED
#define ASIO_NO_DEPRECATED 1
#endif
#ifndef ASIO_NO_EXCEPTIONS
#define ASIO_NO_EXCEPTIONS 1
#endif

// OpenSSL declares a type named UI which collides with Unreal's UI namespace.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#endif

#include <asio.hpp>
#include <asio/ssl.hpp>

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_END
#undef UI

#include "common.generated.h"

INTERNETPROTOCOL_API DECLARE_LOG_CATEGORY_EXTERN(LogAsio, Log, All);

/*
 * With ASIO_NO_EXCEPTIONS the application must provide asio::detail::throw_exception. Asio declares it
 * [[noreturn]] and assumes it never returns, so returning from it is undefined behaviour. The plugin
 * always uses the error_code overloads; if a throwing overload is ever reached it is a programming error,
 * so we terminate in every build configuration (including Shipping).
 */
template <typename Exception>
[[noreturn]] void asio::detail::throw_exception(const Exception& e ASIO_SOURCE_LOCATION_PARAM) {
	LowLevelFatalError(TEXT("InternetProtocol: unhandled Asio error: %hs"), e.what());
	std::abort();
}

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum EOutputExecPins {
	Success,
	Failure,
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class EProtocolType: uint8 {
	V4 = 0,
	V6 = 1,
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class EVerifyMode: uint8 {
	None = 0x00,
	Verify_Peer = 0x01,
	Verify_Fail_If_No_Peer_Cert = 0x02,
	Verify_Client_Once = 0x04,
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class EFileFormat: uint8 {
	asn1 = 0,
	pem = 1,
};

/**
 * TLS configuration.
 * Secure by default: TLS 1.2+, peer verification, system root certificates and (client side) hostname
 * verification + SNI using the address passed to Connect. Verify_Mode = None disables certificate
 * verification and must only be used for local testing; a warning is logged every time it is used.
 */
USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FSecurityContextOpts {
	GENERATED_BODY()

	/** Private key (PEM text or ASN.1 data, see File_Format). */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString Private_Key;

	/** Certificate (PEM text or ASN.1 data, see File_Format). */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString Cert;

	/** Certificate chain, PEM only. */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString Cert_Chain;

	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString RSA_Private_Key;

	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	EFileFormat File_Format = EFileFormat::pem;

	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	EVerifyMode Verify_Mode = EVerifyMode::Verify_Peer;

	/** Overrides the host name used for certificate verification and SNI (client). Empty = use the connect address. */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString Host_Name_Verification;

	/** Additional trusted CA certificate(s), PEM. Useful for private/self-signed CAs. */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	FString CA_Cert;

	/** Trust the platform/engine root certificate store. */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	bool bUseSystemRootCertificates = true;

	/** Client only: verify that the server certificate matches the host name. */
	UPROPERTY(BlueprintReadWrite, Category = "IP|SecurityContextOpts")
	bool bVerifyHostname = true;
};

// HTTP
UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class ERequestMethod : uint8 {
	UNKNOWN = 0,
	DEL = 1,
	GET = 2,
	HEAD = 3,
	OPTIONS = 4,
	PATCH = 5,
	POST = 6,
	PUT = 7
};

USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FHttpRequest {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	ERequestMethod Method = ERequestMethod::GET;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	FString Path = "/";

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	FString Version = "1.1";

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	TMap<FString, FString> Params;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	TMap<FString, FString> Headers;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Request")
	FString Body;
};

USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FHttpResponse {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "IP|Response")
	int Status_Code = 200;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Response")
	FString Status_Message;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Response")
	FString Version = "1.1";

	UPROPERTY(BlueprintReadWrite, Category = "IP|Response")
	TMap<FString, FString> Headers;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Response")
	FString Body;
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class EOpcode: uint8 {
	NONE = 0x00,
	TEXT_FRAME = 0x01,
	BINARY_FRAME = 0x02,
	CLOSE_FRAME = 0x08,
	PING = 0x09,
	PONG = 0x0A
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class ERSV: uint8 {
	NONE = 0x00,
	RSV1 = 0x40,
	RSV2 = 0x20,
	RSV3 = 0x10
};

USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FDataframe {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "IP|DataFrame")
	bool bFin = true;

	UPROPERTY(BlueprintReadWrite, Category = "IP|DataFrame")
	bool bRsv1 = false;

	UPROPERTY(BlueprintReadWrite, Category = "IP|DataFrame")
	bool bRsv2 = false;

	UPROPERTY(BlueprintReadWrite, Category = "IP|DataFrame")
	bool bRsv3 = false;

	/** Ignored on send: clients always mask and servers never mask (RFC 6455 section 5.1). */
	UPROPERTY(BlueprintReadWrite, Category = "IP|DataFrame")
	bool bMask = true;

	EOpcode Opcode = EOpcode::TEXT_FRAME;
	uint64 Length = 0;
	std::array<uint8, 4> masking_key{};
};

UENUM(Blueprintable, Category="IP|ENUM|Common")
enum class ECloseState: uint8  {
	CLOSED = 0,
	CLOSING = 1,
	OPEN = 2
};

// Client side
USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FClientBindOptions {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "IP|ClientBindOptions")
	FString Address = "localhost";

	UPROPERTY(BlueprintReadWrite, Category = "IP|ClientBindOptions")
	FString Port = "8080";

	UPROPERTY(BlueprintReadWrite, Category = "IP|ClientBindOptions")
	EProtocolType Protocol = EProtocolType::V4;
};

// Server side
USTRUCT(BlueprintType, Category="IP|STRUCT|Common")
struct FServerBindOptions {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "IP|ServerBindOptions")
	FString Address = "localhost";

	UPROPERTY(BlueprintReadWrite, Category = "IP|ServerBindOptions")
	int Port = 8080;

	UPROPERTY(BlueprintReadWrite, Category = "IP|ServerBindOptions")
	EProtocolType Protocol = EProtocolType::V4;

	UPROPERTY(BlueprintReadWrite, Category = "IP|ServerBindOptions")
	bool bReuse_Address = true;
};
