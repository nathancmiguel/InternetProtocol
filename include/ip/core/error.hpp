/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"

namespace internetprotocol {
    /// Library specific errors. They integrate with std::error_code (asio::error_code is
    /// std::error_code), so they can be reported through any existing callback.
    enum class errc : int {
        success = 0,
        invalid_state,
        already_connected,
        not_connected,
        invalid_argument,
        timeout,
        queue_full,
        message_too_big,
        protocol_error,
        handshake_failed,
        host_not_found,
        tls_configuration,
        closed_by_peer,
        aborted,
    };

    class error_category_impl final : public std::error_category {
    public:
        const char *name() const noexcept override { return "internetprotocol"; }

        std::string message(int v) const override {
            switch (static_cast<errc>(v)) {
                case errc::success: return "success";
                case errc::invalid_state: return "operation not valid in current state";
                case errc::already_connected: return "already connected or connecting";
                case errc::not_connected: return "not connected";
                case errc::invalid_argument: return "invalid argument";
                case errc::timeout: return "operation timed out";
                case errc::queue_full: return "outgoing queue is full";
                case errc::message_too_big: return "message exceeds configured size limit";
                case errc::protocol_error: return "protocol violation";
                case errc::handshake_failed: return "handshake failed";
                case errc::host_not_found: return "no usable address found for host";
                case errc::tls_configuration: return "invalid TLS configuration";
                case errc::closed_by_peer: return "connection closed by peer";
                case errc::aborted: return "operation aborted";
            }
            return "unknown internetprotocol error";
        }
    };

    inline const std::error_category &ip_category() noexcept {
        static const error_category_impl instance;
        return instance;
    }

    inline std::error_code make_error_code(errc e) noexcept {
        return {static_cast<int>(e), ip_category()};
    }
}

template<>
struct std::is_error_code_enum<internetprotocol::errc> : std::true_type {
};
