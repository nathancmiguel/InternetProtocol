/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"

namespace IPNet {

	/** Errors produced by the plugin itself (as opposed to the OS, Asio or OpenSSL). */
	enum class EError : int {
		NotRunning = 1,
		ProtocolError,
		MessageTooLarge,
		HandshakeFailed,
		TlsSetupFailed,
		QueueFull,
		HttpParseError,
		ConnectionClosed,
		TimedOut,
		InvalidArgument,
		NotFound,
		/** The operation was abandoned by the caller (e.g. the HTTP host changed while a request was in flight). */
		Cancelled,
	};

	const asio::error_category& GetErrorCategory();

	inline asio::error_code MakeError(EError Error) {
		return asio::error_code(static_cast<int>(Error), GetErrorCategory());
	}
}
