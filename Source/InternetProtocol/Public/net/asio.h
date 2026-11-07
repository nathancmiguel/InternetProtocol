/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once
#include "common.h"
#include "asio.generated.h"

USTRUCT(BlueprintType, Category="IP|Net|ErrorCode")
struct INTERNETPROTOCOL_API FErrorCode {
	GENERATED_BODY()
	FErrorCode() = default;
	explicit FErrorCode(const asio::error_code& error_code) {
		if (error_code) {
			Value = error_code.value();
			Message = UTF8_TO_TCHAR(error_code.message().c_str());
		}
	}

	UPROPERTY(BlueprintReadWrite, Category = "IP|ErrorCode")
	FString Message;

	UPROPERTY(BlueprintReadWrite, Category = "IP|ErrorCode")
	int Value = 0;
};

USTRUCT(BlueprintType, Category="IP|Net|Endpoint")
struct INTERNETPROTOCOL_API FUdpEndpoint {
	GENERATED_BODY()
	FUdpEndpoint() = default;
	explicit FUdpEndpoint(const asio::ip::udp::endpoint &ep) {
		const std::string addr = ep.address().to_string();
		Address = UTF8_TO_TCHAR(addr.c_str());
		Port = ep.port();
		Protocol = ep.protocol() == asio::ip::udp::v4() ? EProtocolType::V4 : EProtocolType::V6;
		Endpoint = ep;
	}

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	FString Address;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	int Port = 0;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	EProtocolType Protocol = EProtocolType::V4;

	/** Native endpoint. Only valid when the struct was produced by the plugin; see ToNative(). */
	asio::ip::udp::endpoint Endpoint;

	/**
	 * Returns the endpoint to send to. Structs built in Blueprint only have Address/Port, so those take
	 * precedence over the cached native endpoint when they are set and parse correctly.
	 */
	bool ToNative(asio::ip::udp::endpoint& Out) const {
		if (!Address.IsEmpty()) {
			asio::error_code ec;
			const asio::ip::address addr = asio::ip::make_address(TCHAR_TO_UTF8(*Address), ec);
			if (!ec && Port >= 0 && Port <= 65535) {
				Out = asio::ip::udp::endpoint(addr, static_cast<uint16>(Port));
				return true;
			}
		}
		if (Endpoint.port() != 0) {
			Out = Endpoint;
			return true;
		}
		return false;
	}
};

USTRUCT(BlueprintType, Category="IP|Net|Endpoint")
struct INTERNETPROTOCOL_API FTcpEndpoint {
	GENERATED_BODY()
	FTcpEndpoint() = default;
	explicit FTcpEndpoint(const asio::ip::tcp::endpoint &ep) {
		const std::string addr = ep.address().to_string();
		Address = UTF8_TO_TCHAR(addr.c_str());
		Port = ep.port();
		Protocol = ep.protocol() == asio::ip::tcp::v4() ? EProtocolType::V4 : EProtocolType::V6;
		Endpoint = ep;
	}

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	FString Address;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	int Port = 0;

	UPROPERTY(BlueprintReadWrite, Category = "IP|Endpoint")
	EProtocolType Protocol = EProtocolType::V4;

	asio::ip::tcp::endpoint Endpoint;
};
