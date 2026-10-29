/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/remote.hpp"
#include "ip/utils/httpparser.hpp"

namespace internetprotocol {
    /**
     * Server-side HTTP/1.x connection. Parses requests incrementally (Content-Length / chunked,
     * size limits), supports keep-alive with strictly ordered responses and answers malformed
     * requests with the proper 4xx/5xx status.
     *
     * Usage inside a request handler: fill `response` and call `write()`.
     * Requests received while a response is still pending are buffered, never dispatched concurrently.
     */
    template<typename Stream>
    class basic_http_remote : public remote_core<Stream>, public std::enable_shared_from_this<basic_http_remote<Stream>> {
    public:
        using conn_t = connection<Stream>;
        using write_callback = std::function<void(const asio::error_code &, const size_t bytes_sent)>;

        explicit basic_http_remote(std::shared_ptr<conn_t> c, http_limits_t limits = {})
            : remote_core<Stream>(std::move(c)), limits_(limits), parser_(http_parser::kind::request, limits) {}

        /// Response to be sent by write(). Reset to `200 OK` before every request.
        http_response_t response;
        /// Legacy accessor: returns `response` (kept for source compatibility).
        http_response_t &headers() { return response; }

        /// Sends `response` and (if the client asked for it) closes the connection afterwards.
        bool write(const write_callback &callback = nullptr) {
            if (!this->conn_->is_open()) return false;
            http_response_t res = response;
            if (will_close_) res.headers["Connection"] = "close";
            else if (!find_header(res.headers, "connection") && request_version_ == "1.0") res.headers["Connection"] = "keep-alive";
            const std::string payload = prepare_response(res, head_request_);
            const bool close_after = will_close_;
            if (close_after) dead_.store(true);
            const bool ok = this->conn_->write(std::string_view(payload), callback);
            if (ok && close_after) this->conn_->close();
            auto self = this->shared_from_this();
            asio::post(this->conn_->get_executor(), [self] {
                self->busy_ = false;
                self->drain();
            });
            return ok;
        }

        event<void(const http_request_t &)> on_request;
        event<void()> on_close;
        event<void(const asio::error_code &)> on_error;

        /// Internal: begins reading. Called by the server after on_client_accepted.
        void start() {
            this->conn_->set_handlers(
                [w = this->weak_from_this()](const uint8_t *p, std::size_t n) {
                    if (auto s = w.lock()) s->on_data(p, n);
                },
                [w = this->weak_from_this()](const std::error_code &ec) {
                    auto s = w.lock();
                    if (!s) return;
                    if (ec) { s->set_error(ec); s->on_error(ec); }
                    s->on_close();
                    if (s->internal_on_closed) s->internal_on_closed();
                });
            this->conn_->start();
        }

    private:
        http_limits_t limits_;
        http_parser parser_;
        std::string pending_;
        bool busy_ = false;
        std::atomic<bool> dead_{false};
        bool will_close_ = false;
        bool head_request_ = false;
        std::string request_version_ = "1.1";

        void on_data(const uint8_t *p, std::size_t n) {
            if (dead_.load()) return;
            if (busy_) {
                if (pending_.size() + n > limits_.max_header_bytes + limits_.max_body_bytes) {
                    this->conn_->abort(make_error_code(errc::message_too_big));
                    return;
                }
                pending_.append(reinterpret_cast<const char *>(p), n);
                return;
            }
            process(p, n);
        }

        void drain() {
            while (!busy_ && !dead_.load() && !pending_.empty() && this->conn_->is_open()) {
                std::string chunk;
                chunk.swap(pending_);
                process(reinterpret_cast<const uint8_t *>(chunk.data()), chunk.size());
            }
        }

        void process(const uint8_t *p, std::size_t n) {
            std::size_t off = 0;
            while (off < n && !busy_ && !dead_.load()) {
                std::error_code ec;
                off += parser_.feed(p + off, n - off, ec);
                if (ec) {
                    reject(ec == errc::message_too_big ? (parser_.in_progress() ? 431 : 413) : 400);
                    return;
                }
                if (!parser_.done()) break;
                dispatch();
                parser_.reset();
            }
            if (off < n && busy_) pending_.append(reinterpret_cast<const char *>(p + off), n - off);
        }

        void reject(int status) {
            will_close_ = true;
            response = {};
            response.status_code = status;
            response.body = status_text(status);
            response.headers["Content-Type"] = "text/plain";
            busy_ = true;
            write();
            this->conn_->close();
        }

        void dispatch() {
            http_request_t &req = parser_.request();
            response = {};
            response.status_code = 200;
            response.status_message = "OK";
            response.headers["Content-Type"] = "text/plain";
            response.headers["X-Powered-By"] = "ASIO";
            request_version_ = req.version;
            head_request_ = req.method == HEAD;
            if (req.version != "1.0" && req.version != "1.1") { reject(505); return; }
            if (parser_.method_unknown_) {
                will_close_ = true;
                response.status_code = 501;
                response.status_message.clear();
                response.body = "Method not supported.";
                response.headers["Allow"] = "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT";
                busy_ = true;
                write();
                return;
            }
            if (req.version == "1.0") will_close_ = !header_has_token(req.headers, "connection", "keep-alive");
            else will_close_ = header_has_token(req.headers, "connection", "close");
            busy_ = true; // until write() is called
            if (!on_request) {
                response.status_code = 404;
                response.status_message.clear();
                response.body = "Not Found";
                write();
                return;
            }
            on_request(req);
        }
    };

    using http_remote_c = basic_http_remote<plain_stream>;
#ifdef IP_ENABLE_SSL
    using http_remote_ssl_c = basic_http_remote<ssl_stream>;
#endif
}
