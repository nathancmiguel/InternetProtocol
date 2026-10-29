/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"

namespace internetprotocol {
    inline bool is_space_char(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }

    inline char to_lower_char(char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// Trims ASCII whitespace on both ends. Safe on empty / all-whitespace strings.
    inline std::string trim_whitespace(std::string_view str) {
        size_t b = 0, e = str.size();
        while (b < e && is_space_char(str[b])) ++b;
        while (e > b && is_space_char(str[e - 1])) --e;
        return std::string(str.substr(b, e - b));
    }

    inline std::string buffer_to_string(const std::vector<uint8_t> &buf) {
        return std::string(buf.begin(), buf.end());
    }

    inline std::vector<uint8_t> string_to_buffer(std::string_view str) {
        return std::vector<uint8_t>(str.begin(), str.end());
    }

    /// Splits `str` on `delimiter` (the whole string is trimmed first). Empty trailing token is dropped.
    inline std::vector<std::string> split_string(const std::string &str, char delimiter) {
        std::vector<std::string> tokens;
        const std::string trimmed = trim_whitespace(str);
        size_t start = 0;
        while (start <= trimmed.size()) {
            size_t pos = trimmed.find(delimiter, start);
            if (pos == std::string::npos) pos = trimmed.size();
            if (pos > start || pos < trimmed.size()) tokens.emplace_back(trimmed.substr(start, pos - start));
            start = pos + 1;
        }
        return tokens;
    }

    /// In-place ASCII lower-casing.
    inline void string_to_lower(std::string &str) {
        for (char &c: str) c = to_lower_char(c);
    }

    inline std::string to_lower_copy(std::string_view str) {
        std::string s(str);
        string_to_lower(s);
        return s;
    }

    inline bool iequals(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (to_lower_char(a[i]) != to_lower_char(b[i])) return false;
        return true;
    }

    /// Case-insensitive header lookup (map keys may be in any case). Returns nullptr when missing.
    inline const std::string *find_header(const std::map<std::string, std::string> &headers,
                                          std::string_view name) {
        auto it = headers.find(std::string(name));
        if (it != headers.end()) return &it->second;
        for (const auto &kv: headers)
            if (iequals(kv.first, name)) return &kv.second;
        return nullptr;
    }

    /// Does the comma separated header value contain `token` (case-insensitive)?
    inline bool header_has_token(const std::map<std::string, std::string> &headers,
                                 std::string_view name, std::string_view token) {
        const std::string *v = find_header(headers, name);
        if (!v) return false;
        size_t start = 0;
        while (start <= v->size()) {
            size_t pos = v->find(',', start);
            if (pos == std::string::npos) pos = v->size();
            if (iequals(trim_whitespace(std::string_view(*v).substr(start, pos - start)), token)) return true;
            start = pos + 1;
        }
        return false;
    }

    inline std::string url_encode(std::string_view in) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(in.size());
        for (unsigned char c: in) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            }
        }
        return out;
    }

    inline std::string url_decode(std::string_view in, bool plus_as_space = false) {
        auto hexv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size() + 0 && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) {
                out.push_back(static_cast<char>(hexv(in[i + 1]) * 16 + hexv(in[i + 2])));
                i += 2;
            } else if (plus_as_space && in[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(in[i]);
            }
        }
        return out;
    }

    /// Strict UTF-8 validation (RFC 3629): rejects overlongs, surrogates and > U+10FFFF.
    inline bool is_valid_utf8(const uint8_t *p, size_t n) noexcept {
        size_t i = 0;
        while (i < n) {
            const uint8_t c = p[i];
            if (c < 0x80) { ++i; continue; }
            size_t len;
            uint32_t cp;
            if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
            else return false;
            if (i + len > n) return false;
            for (size_t k = 1; k < len; ++k) {
                if ((p[i + k] & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (p[i + k] & 0x3F);
            }
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            i += len;
        }
        return true;
    }
}
