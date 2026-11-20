/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "net/errors.h"
#include "net/asio.h"

FErrorCode::FErrorCode(const asio::error_code& error_code) {
	if (!error_code) {
		return;
	}
	Value = error_code.value();
	Message = UTF8_TO_TCHAR(error_code.message().c_str());
	const asio::error_category& Cat = error_code.category();
	if (&Cat == &IPNet::GetErrorCategory()) {
		const IPNet::EError Plugin = static_cast<IPNet::EError>(Value);
		Category = Plugin == IPNet::EError::HttpParseError ? EErrorCategory::Http : EErrorCategory::Plugin;
	} else if (&Cat == &asio::error::get_ssl_category() || &Cat == &asio::ssl::error::get_stream_category()) {
		Category = EErrorCategory::SSL;
	} else {
		Category = EErrorCategory::System;
	}
}

namespace IPNet {
	namespace {
		class FErrorCategory final : public asio::error_category {
		public:
			const char* name() const noexcept override { return "internet_protocol"; }

			std::string message(int Value) const override {
				switch (static_cast<EError>(Value)) {
				case EError::NotRunning: return "Network service is not running";
				case EError::ProtocolError: return "Protocol error";
				case EError::MessageTooLarge: return "Message too large";
				case EError::HandshakeFailed: return "Handshake failed";
				case EError::TlsSetupFailed: return "TLS configuration failed";
				case EError::QueueFull: return "Send queue is full";
				case EError::HttpParseError: return "Malformed HTTP message";
				case EError::ConnectionClosed: return "Connection closed";
				case EError::TimedOut: return "Operation timed out";
				case EError::InvalidArgument: return "Invalid argument";
				case EError::NotFound: return "Not found";
				case EError::Cancelled: return "Operation cancelled";
				default: return "Unknown error";
				}
			}
		};
	}

	const asio::error_category& GetErrorCategory() {
		static FErrorCategory Category;
		return Category;
	}
}
