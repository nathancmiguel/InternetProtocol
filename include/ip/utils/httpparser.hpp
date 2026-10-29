/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/types.hpp"
#include "ip/utils/net.hpp"

namespace internetprotocol {
    /**
     * Incremental, allocation-light HTTP/1.x message parser (requests and responses).
     *
     * Supports Content-Length, chunked transfer coding (+trailers) and read-until-close bodies,
     * enforces header/body limits and rejects the classic request-smuggling shapes
     * (Content-Length + Transfer-Encoding, duplicated/non-numeric Content-Length, bad framing).
     *
     * feed() consumes bytes only up to the end of the current message, so pipelined data is left
     * to the caller: loop `while (n) { used = p.feed(...); if (p.done()) {...; p.reset();} }`.
     */
    class http_parser {
    public:
        enum class kind { request, response };
        enum class phase { head, body_fixed, chunk_size, chunk_data, chunk_crlf, trailers, body_eof, done, failed };

        explicit http_parser(kind k, http_limits_t limits = {}) : kind_(k), limits_(limits) {}

        void reset() {
            phase_ = phase::head;
            head_.clear();
            remaining_ = 0;
            line_.clear();
            request_ = {};
            response_ = {};
            head_response_ = false;
        }

        /// Response to a HEAD request: no body even if Content-Length is present.
        void expect_no_body(bool v) { head_response_ = v; }

        bool done() const { return phase_ == phase::done; }

        bool failed() const { return phase_ == phase::failed; }

        bool reads_until_close() const { return phase_ == phase::body_eof; }

        bool in_progress() const { return phase_ != phase::head || !head_.empty(); }

        http_request_t &request() { return request_; }

        http_response_t &response() { return response_; }

        /// Signals EOF. Completes a read-until-close body; otherwise reports truncation.
        void on_eof(std::error_code &ec) {
            if (phase_ == phase::body_eof) { phase_ = phase::done; return; }
            if (phase_ != phase::done && in_progress()) ec = errc::closed_by_peer;
        }

        size_t feed(const uint8_t *data, size_t n, std::error_code &ec) {
            size_t used = 0;
            while (used < n && phase_ != phase::done && phase_ != phase::failed) {
                switch (phase_) {
                    case phase::head: used += feed_head(data + used, n - used, ec); break;
                    case phase::body_fixed: used += feed_fixed(data + used, n - used, ec); break;
                    case phase::body_eof: used += feed_eof(data + used, n - used, ec); break;
                    case phase::chunk_size:
                    case phase::chunk_crlf:
                    case phase::trailers: used += feed_line(data + used, n - used, ec); break;
                    case phase::chunk_data: used += feed_chunk(data + used, n - used, ec); break;
                    default: break;
                }
                if (ec) { phase_ = phase::failed; break; }
            }
            return used;
        }

    private:
        kind kind_;
        http_limits_t limits_;
        phase phase_ = phase::head;
        std::string head_;
        std::string line_;
        uint64_t remaining_ = 0;
        bool head_response_ = false;
        http_request_t request_;
        http_response_t response_;

        std::string &body() { return kind_ == kind::request ? request_.body : response_.body; }

        std::map<std::string, std::string> &hdrs() { return kind_ == kind::request ? request_.headers : response_.headers; }

        bool check_body_size(uint64_t add, std::error_code &ec) {
            if (body().size() + add > limits_.max_body_bytes) { ec = errc::message_too_big; return false; }
            return true;
        }

        size_t feed_head(const uint8_t *d, size_t n, std::error_code &ec) {
            // Append byte-wise until CRLFCRLF; leading CRLFs before a message are tolerated (RFC 9112 §2.2).
            size_t i = 0;
            for (; i < n; ++i) {
                const char c = static_cast<char>(d[i]);
                if (head_.empty() && (c == '\r' || c == '\n')) continue;
                head_.push_back(c);
                if (head_.size() > limits_.max_header_bytes) { ec = errc::message_too_big; return i + 1; }
                if (head_.size() >= 4 && head_.compare(head_.size() - 4, 4, "\r\n\r\n") == 0) {
                    ++i;
                    parse_head(ec);
                    return i;
                }
            }
            return i;
        }

        void parse_head(std::error_code &ec) {
            const size_t first_end = head_.find("\r\n");
            const std::string start_line = head_.substr(0, first_end);
            if (!parse_start_line(start_line)) { ec = errc::protocol_error; return; }
            size_t pos = first_end + 2;
            while (pos < head_.size()) {
                size_t eol = head_.find("\r\n", pos);
                if (eol == std::string::npos || eol == pos) break;
                std::string_view line(head_.data() + pos, eol - pos);
                const size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0 || is_space_char(line[colon - 1]) ||
                    is_space_char(line[0])) { ec = errc::protocol_error; return; }
                detail::append_header(hdrs(), line);
                pos = eol + 2;
            }
            head_.clear();
            head_.shrink_to_fit();
            decide_body(ec);
        }

        bool parse_start_line(const std::string &line) {
            if (kind_ == kind::request) {
                const size_t a = line.find(' ');
                const size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
                if (a == std::string::npos || b == std::string::npos || a == 0) return false;
                const std::string method = line.substr(0, a);
                const std::string target = line.substr(a + 1, b - a - 1);
                const std::string ver = line.substr(b + 1);
                if (ver.size() < 8 || ver.compare(0, 5, "HTTP/") != 0 || target.empty()) return false;
                request_.method = string_to_request_method(method);
                request_.version = ver.substr(5);
                const size_t q = target.find('?');
                request_.path = url_decode(target.substr(0, q));
                if (q != std::string::npos) {
                    std::string_view query(target.data() + q + 1, target.size() - q - 1);
                    size_t s = 0;
                    while (s <= query.size() && !query.empty()) {
                        size_t amp = query.find('&', s);
                        if (amp == std::string_view::npos) amp = query.size();
                        std::string_view kv = query.substr(s, amp - s);
                        if (!kv.empty()) {
                            const size_t eq = kv.find('=');
                            request_.params[url_decode(kv.substr(0, eq), true)] =
                                    eq == std::string_view::npos ? std::string() : url_decode(kv.substr(eq + 1), true);
                        }
                        s = amp + 1;
                    }
                }
                method_unknown_ = request_.method == UNKNOWN;
                raw_method_ = method;
                return true;
            }
            if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0) return false;
            const size_t sp = line.find(' ');
            if (sp == std::string::npos || line.size() < sp + 4) return false;
            response_.version = line.substr(5, sp - 5);
            const std::string code = line.substr(sp + 1, 3);
            for (char c: code) if (c < '0' || c > '9') return false;
            response_.status_code = std::stoi(code);
            response_.status_message = line.size() > sp + 5 ? line.substr(sp + 5) : std::string();
            return true;
        }

    public:
        bool method_unknown_ = false;
        std::string raw_method_;

    private:
        void decide_body(std::error_code &ec) {
            const auto &h = hdrs();
            const std::string *te = find_header(h, "transfer-encoding");
            const std::string *cl = find_header(h, "content-length");
            if (te && cl) { ec = errc::protocol_error; return; }
            const bool no_body_response = kind_ == kind::response &&
                                          (head_response_ || response_.status_code < 200 ||
                                           response_.status_code == 204 || response_.status_code == 304);
            if (no_body_response) { phase_ = phase::done; return; }
            if (te) {
                if (!header_has_token(h, "transfer-encoding", "chunked")) { ec = errc::protocol_error; return; }
                phase_ = phase::chunk_size;
                return;
            }
            if (cl) {
                if (cl->empty() || cl->size() > 18) { ec = errc::protocol_error; return; }
                uint64_t v = 0;
                for (char c: *cl) {
                    if (c < '0' || c > '9') { ec = errc::protocol_error; return; }
                    v = v * 10 + static_cast<uint64_t>(c - '0');
                }
                if (v > limits_.max_body_bytes) { ec = errc::message_too_big; return; }
                remaining_ = v;
                if (v > 0) body().reserve(static_cast<size_t>(v));
                phase_ = v == 0 ? phase::done : phase::body_fixed;
                return;
            }
            phase_ = kind_ == kind::response ? phase::body_eof : phase::done;
        }

        size_t feed_fixed(const uint8_t *d, size_t n, std::error_code &ec) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining_, n));
            if (!check_body_size(take, ec)) return 0;
            body().append(reinterpret_cast<const char *>(d), take);
            remaining_ -= take;
            if (remaining_ == 0) phase_ = phase::done;
            return take;
        }

        size_t feed_eof(const uint8_t *d, size_t n, std::error_code &ec) {
            if (!check_body_size(n, ec)) return 0;
            body().append(reinterpret_cast<const char *>(d), n);
            return n;
        }

        size_t feed_chunk(const uint8_t *d, size_t n, std::error_code &ec) {
            const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining_, n));
            if (!check_body_size(take, ec)) return 0;
            body().append(reinterpret_cast<const char *>(d), take);
            remaining_ -= take;
            if (remaining_ == 0) phase_ = phase::chunk_crlf;
            return take;
        }

        // Handles chunk-size lines, the CRLF after chunk data and trailer lines.
        size_t feed_line(const uint8_t *d, size_t n, std::error_code &ec) {
            size_t i = 0;
            for (; i < n; ++i) {
                const char c = static_cast<char>(d[i]);
                line_.push_back(c);
                if (line_.size() > 4096) { ec = errc::message_too_big; return i + 1; }
                if (c != '\n') continue;
                ++i;
                if (line_.size() < 2 || line_[line_.size() - 2] != '\r') { ec = errc::protocol_error; return i; }
                line_.resize(line_.size() - 2);
                std::string l = std::move(line_);
                line_.clear();
                if (phase_ == phase::chunk_size) {
                    const size_t semi = l.find(';');
                    std::string hex = trim_whitespace(l.substr(0, semi));
                    if (hex.empty() || hex.size() > 15) { ec = errc::protocol_error; return i; }
                    uint64_t v = 0;
                    for (char h: hex) {
                        int dv = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (dv < 0) { ec = errc::protocol_error; return i; }
                        v = v * 16 + static_cast<uint64_t>(dv);
                    }
                    if (v == 0) phase_ = phase::trailers;
                    else { remaining_ = v; phase_ = phase::chunk_data; }
                } else if (phase_ == phase::chunk_crlf) {
                    if (!l.empty()) { ec = errc::protocol_error; return i; }
                    phase_ = phase::chunk_size;
                } else { // trailers
                    if (l.empty()) phase_ = phase::done;
                    else detail::append_header(hdrs(), l);
                }
                return i; // phase changed: let feed() dispatch again
            }
            return i;
        }
    };
}
