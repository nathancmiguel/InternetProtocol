/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/types.hpp"
#include "ip/utils/string.hpp"

namespace internetprotocol {
    inline std::string request_method_to_string(request_method_e method) {
        switch (method) {
            case DEL: return "DELETE";
            case GET: return "GET";
            case HEAD: return "HEAD";
            case OPTIONS: return "OPTIONS";
            case PATCH: return "PATCH";
            case POST: return "POST";
            case PUT: return "PUT";
            default: return "";
        }
    }

    inline request_method_e string_to_request_method(const std::string &method_str) {
        static const std::unordered_map<std::string, request_method_e> method_map = {
            {"DELETE", DEL}, {"GET", GET}, {"HEAD", HEAD}, {"OPTIONS", OPTIONS},
            {"PATCH", PATCH}, {"POST", POST}, {"PUT", PUT}
        };
        auto it = method_map.find(method_str);
        return it != method_map.end() ? it->second : UNKNOWN;
    }

    namespace detail {
        /// Header injection guard: CR/LF/NUL can never reach the wire inside a field.
        inline std::string sanitize_field(std::string_view s) {
            std::string out(s);
            for (char &c: out)
                if (c == '\r' || c == '\n' || c == '\0') c = ' ';
            return out;
        }

        inline std::string encode_target(const http_request_t &req) {
            std::string path = req.path.empty() ? "/" : req.path;
            std::string out;
            out.reserve(path.size());
            for (unsigned char c: path) {
                if (c <= 0x20 || c == 0x7F) { // space and control chars
                    static constexpr char hex[] = "0123456789ABCDEF";
                    out.push_back('%');
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else out.push_back(static_cast<char>(c));
            }
            if (!req.params.empty()) {
                out.push_back(out.find('?') == std::string::npos ? '?' : '&');
                bool first = true;
                for (const auto &p: req.params) {
                    if (!first) out.push_back('&');
                    out += url_encode(p.first) + "=" + url_encode(p.second);
                    first = false;
                }
            }
            return out;
        }
    }

    /// Serialises a request. `Host` is added if the caller did not provide one;
    /// `Content-Length` is generated whenever a body exists and the caller did not set it.
    inline std::string prepare_request(const http_request_t &req, const std::string &address, const uint16_t port) {
        std::string payload;
        payload.reserve(256 + req.body.size());
        payload += request_method_to_string(req.method);
        payload += ' ';
        payload += detail::encode_target(req);
        payload += " HTTP/" + detail::sanitize_field(req.version) + "\r\n";

        if (!find_header(req.headers, "host")) {
            payload += "Host: " + detail::sanitize_field(address);
            if (port != 80 && port != 443 && port != 0) payload += ":" + std::to_string(port);
            payload += "\r\n";
        }
        for (const auto &h: req.headers)
            payload += detail::sanitize_field(h.first) + ": " + detail::sanitize_field(h.second) + "\r\n";
        if (!req.body.empty() && !find_header(req.headers, "content-length") &&
            !find_header(req.headers, "transfer-encoding"))
            payload += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        payload += "\r\n";
        payload += req.body;
        return payload;
    }

    /// Serialises a response. `head_only` omits the body (HEAD) but keeps the Content-Length.
    inline std::string prepare_response(const http_response_t &res, bool head_only = false) {
        std::string payload;
        payload.reserve(256 + res.body.size());
        payload += "HTTP/" + detail::sanitize_field(res.version) + " " + std::to_string(res.status_code) + " ";
        payload += res.status_message.empty() ? status_text(res.status_code) : detail::sanitize_field(res.status_message);
        payload += "\r\n";
        for (const auto &h: res.headers)
            payload += detail::sanitize_field(h.first) + ": " + detail::sanitize_field(h.second) + "\r\n";
        const bool no_body_status = res.status_code < 200 || res.status_code == 204 || res.status_code == 304;
        if (!no_body_status && !find_header(res.headers, "content-length") &&
            !find_header(res.headers, "transfer-encoding"))
            payload += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
        payload += "\r\n";
        if (!head_only && !no_body_status) payload += res.body;
        return payload;
    }

    namespace detail {
        inline void append_header(std::map<std::string, std::string> &headers, std::string_view line) {
            const size_t pos = line.find(':');
            if (pos == std::string_view::npos || pos == 0) return;
            std::string key = trim_whitespace(line.substr(0, pos));
            if (key.empty()) return;
            string_to_lower(key);
            std::string value = trim_whitespace(line.substr(pos + 1));
            auto it = headers.find(key);
            if (it == headers.end()) headers.emplace(std::move(key), std::move(value));
            else if (!value.empty()) it->second += ", " + value; // RFC 9110 §5.3 list folding
        }
    }

    /// Parses one `Name: value` line into `req.headers` (key is lower-cased, duplicates folded).
    inline void req_append_header(http_request_t &req, const std::string &headerline) {
        detail::append_header(req.headers, headerline);
    }

    inline void res_append_header(http_response_t &res, const std::string &headerline) {
        detail::append_header(res.headers, headerline);
    }
}
