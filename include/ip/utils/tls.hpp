/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/types.hpp"

#ifdef IP_ENABLE_SSL
#include <openssl/ssl.h>

namespace internetprotocol {
    /// Loads certificates/keys from memory into `ctx`. Never throws: returns the first error.
    inline std::error_code configure_ssl_context(asio::ssl::context &ctx, const security_context_opts &o, bool server) {
        std::error_code ec;
        ctx.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                        asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                        asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use, ec);
        if (ec) return ec;
        auto buf = [](const std::string &s) { return asio::const_buffer(s.data(), s.size()); };
        if (!o.cert_chain.empty()) { ctx.use_certificate_chain(buf(o.cert_chain), ec); if (ec) return ec; }
        if (!o.cert.empty()) { ctx.use_certificate(buf(o.cert), o.file_format, ec); if (ec) return ec; }
        if (!o.private_key.empty()) { ctx.use_private_key(buf(o.private_key), o.file_format, ec); if (ec) return ec; }
        if (!o.rsa_private_key.empty()) { ctx.use_rsa_private_key(buf(o.rsa_private_key), o.file_format, ec); if (ec) return ec; }
        if (!o.ca_cert.empty()) { ctx.add_certificate_authority(buf(o.ca_cert), ec); if (ec) return ec; }
        if (!server && o.use_default_verify_paths && o.verify_mode != none) { ctx.set_default_verify_paths(ec); if (ec) return ec; }
        int flags = asio::ssl::verify_none;
        if (o.verify_mode & verify_peer) flags |= asio::ssl::verify_peer;
        if (o.verify_mode & verify_fail_if_no_peer_cert) flags |= asio::ssl::verify_fail_if_no_peer_cert;
        if (o.verify_mode & verify_client_once) flags |= asio::ssl::verify_client_once;
        ctx.set_verify_mode(flags, ec);
        return ec;
    }

    inline std::shared_ptr<asio::ssl::context> make_client_ssl_context(const security_context_opts &o, std::error_code &ec) {
        auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
        ec = configure_ssl_context(*ctx, o, false);
        return ctx;
    }

    inline std::shared_ptr<asio::ssl::context> make_server_ssl_context(const security_context_opts &o, std::error_code &ec) {
        auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
        ec = configure_ssl_context(*ctx, o, true);
        return ctx;
    }
}
#endif
