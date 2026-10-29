/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/types.hpp"
#include "ip/utils/dataframe.hpp"
#include "ip/utils/string.hpp"

namespace internetprotocol {
    inline constexpr const char *magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    inline constexpr const char *base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// SHA-1 (RFC 3174). Only used for the WebSocket accept key. No global state, no signed-shift UB.
    inline std::array<uint8_t, 20> sha1(const std::string &input) {
        uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        std::string msg = input;
        const uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8u;
        msg.push_back(static_cast<char>(0x80));
        while (msg.size() % 64 != 56) msg.push_back('\0');
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFF));

        auto rol = [](uint32_t v, int s) -> uint32_t { return (v << s) | (v >> (32 - s)); };
        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[80];
            for (int j = 0; j < 16; ++j) {
                const auto *p = reinterpret_cast<const unsigned char *>(msg.data() + off + j * 4);
                w[j] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            }
            for (int j = 16; j < 80; ++j) w[j] = rol(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int j = 0; j < 80; ++j) {
                uint32_t f, k;
                if (j < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
                else if (j < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                const uint32_t t = rol(a, 5) + f + e + k + w[j];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        std::array<uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = uint8_t(h[i] >> 24);
            out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8);
            out[i * 4 + 3] = uint8_t(h[i]);
        }
        return out;
    }

    inline std::string base64_encode(const uint8_t *input, size_t length) {
        std::string out;
        out.reserve((length + 2) / 3 * 4);
        size_t i = 0;
        while (length - i >= 3) {
            const uint32_t t = (uint32_t(input[i]) << 16) | (uint32_t(input[i + 1]) << 8) | input[i + 2];
            out.push_back(base64_chars[(t >> 18) & 63]);
            out.push_back(base64_chars[(t >> 12) & 63]);
            out.push_back(base64_chars[(t >> 6) & 63]);
            out.push_back(base64_chars[t & 63]);
            i += 3;
        }
        if (const size_t rem = length - i; rem > 0) {
            const uint32_t t = (uint32_t(input[i]) << 16) | (rem > 1 ? uint32_t(input[i + 1]) << 8 : 0u);
            out.push_back(base64_chars[(t >> 18) & 63]);
            out.push_back(base64_chars[(t >> 12) & 63]);
            out.push_back(rem == 2 ? base64_chars[(t >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    inline std::string generate_accept_key(const std::string &sec_websocket_key) {
        const auto hash = sha1(sec_websocket_key + magic_string);
        return base64_encode(hash.data(), hash.size());
    }

    /// Random 16-byte nonce, base64 encoded (RFC 6455 §4.1: must be fresh per connection).
    inline std::string generate_websocket_key() {
        std::array<uint8_t, 16> nonce{};
        const auto a = mask_gen(), b = mask_gen(), c = mask_gen(), d = mask_gen();
        std::copy(a.begin(), a.end(), nonce.begin());
        std::copy(b.begin(), b.end(), nonce.begin() + 4);
        std::copy(c.begin(), c.end(), nonce.begin() + 8);
        std::copy(d.begin(), d.end(), nonce.begin() + 12);
        return base64_encode(nonce.data(), nonce.size());
    }

    /// Validates an upgrade request. On failure `res_handshake.body` describes the problem.
    inline bool validate_handshake_request(const http_request_t &req, http_response_t &res) {
        auto fail = [&](const char *msg) { res.body = msg; return false; };
        if (req.method != GET) return fail("Handshake must use the GET method");
        if (!header_has_token(req.headers, "connection", "upgrade")) return fail("\"Connection\" header must contain \"Upgrade\"");
        const std::string *up = find_header(req.headers, "upgrade");
        if (!up) return fail("\"Upgrade\" header not found");
        if (!iequals(trim_whitespace(*up), "websocket")) return fail("\"Upgrade\" header value is not \"websocket\"");
        const std::string *key = find_header(req.headers, "sec-websocket-key");
        if (!key || key->empty()) return fail("\"Sec-WebSocket-Key\" header not found");
        const std::string *ver = find_header(req.headers, "sec-websocket-version");
        if (!ver) return fail("\"Sec-WebSocket-Version\" header not found");
        if (trim_whitespace(*ver) != "13") return fail("Invalid \"Sec-WebSocket-Version\" header value");
        return true;
    }

    /// Validates the server's 101 answer against the request we sent.
    inline bool validate_handshake_response(const http_request_t &req, http_response_t &res) {
        auto fail = [&](const char *msg) { res.body = msg; return false; };
        if (res.status_code != 101) return fail("Server did not switch protocols (status is not 101)");
        if (!header_has_token(res.headers, "connection", "upgrade")) return fail("\"Connection\" header must contain \"Upgrade\"");
        const std::string *up = find_header(res.headers, "upgrade");
        if (!up || !iequals(trim_whitespace(*up), "websocket")) return fail("\"Upgrade\" header is not \"websocket\"");
        const std::string *acc = find_header(res.headers, "sec-websocket-accept");
        if (!acc) return fail("Sec-WebSocket-Accept header not found");
        const std::string *key = find_header(req.headers, "sec-websocket-key");
        if (!key) return fail("Request has no Sec-WebSocket-Key");
        if (generate_accept_key(trim_whitespace(*key)) != trim_whitespace(*acc))
            return fail("Invalid handshake: \"Sec-WebSocket-Accept\" is invalid.");
        return true;
    }
}
