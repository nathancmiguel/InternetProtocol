/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/remote.hpp"
#include "ip/utils/httpparser.hpp"
#include "ip/websocket/wsengine.hpp"

namespace internetprotocol {
    /**
     * Server-side WebSocket connection: validates the HTTP upgrade, then speaks RFC 6455
     * (fragmentation, ping/pong with automatic replies, close handshake, UTF-8 checks, size limits).
     */
    template<typename Stream>
    class basic_ws_remote : public remote_core<Stream>, public std::enable_shared_from_this<basic_ws_remote<Stream>> {
    public:
        using conn_t = connection<Stream>;
        using write_callback = std::function<void(const asio::error_code &, const size_t)>;

        explicit basic_ws_remote(std::shared_ptr<conn_t> c, std::shared_ptr<ws_settings> settings = nullptr,
                                 std::chrono::milliseconds handshake_timeout = std::chrono::milliseconds(10000))
            : remote_core<Stream>(std::move(c)), settings_(settings ? std::move(settings) : std::make_shared<ws_settings>()),
              handshake_timeout_(handshake_timeout), parser_(http_parser::kind::request) {}

        bool is_open() const noexcept { return state() == OPEN && this->conn_->is_open(); }

        close_state_e state() const {
            std::lock_guard l(m_);
            return eng_ ? eng_->state() : (handshaking_ ? CLOSING : CLOSED);
        }

        /// Max accepted message size (bytes) and automatic ping interval (0 = off). Thread-safe.
        void set_max_message_size(std::size_t bytes) { settings_->max_message_bytes.store(bytes); }

        void set_ping_interval(uint16_t seconds) { settings_->ping_interval_seconds.store(seconds); }

        bool write(const std::string &message, const dataframe_t &dataframe = {}, const write_callback &callback = nullptr) {
            auto e = engine();
            return e && e->send_message(reinterpret_cast<const uint8_t *>(message.data()), message.size(), false, dataframe, callback);
        }

        bool write_buffer(const std::vector<uint8_t> &buffer, const dataframe_t &dataframe = {}, const write_callback &callback = nullptr) {
            auto e = engine();
            return e && e->send_message(buffer.data(), buffer.size(), true, dataframe, callback);
        }

        bool ping(const write_callback &callback = nullptr) {
            auto e = engine();
            return e && e->send_control(PING, {}, callback);
        }

        bool pong(const write_callback &callback = nullptr) {
            auto e = engine();
            return e && e->send_control(PONG, {}, callback);
        }

        /// Kept for API compatibility: the server starts the handshake automatically after on_client_accepted.
        void connect() {}

        /// Closing handshake: sends CLOSE and waits (bounded) for the peer's CLOSE before dropping TCP.
        void end(const uint16_t code = 1000, const std::string &reason = "") {
            if (auto e = engine()) e->close(code, reason, true);
            else this->conn_->close();
        }

        /// Sends CLOSE, flushes and closes the TCP connection without waiting for the peer.
        void close(const uint16_t code = 1000, const std::string &reason = "") {
            if (auto e = engine()) e->close(code, reason, false);
            else this->conn_->close();
        }

        event<void(const http_request_t &)> on_connected;
        event<void(const http_request_t &)> on_unexpected_handshake;
        event<void(const std::vector<uint8_t> &, bool)> on_message_received;
        event<void()> on_ping;
        event<void()> on_pong;
        event<void(const uint16_t, const std::string &)> on_close;
        event<void(const asio::error_code &)> on_error;

        /// Internal: begins the handshake. Called by the server after on_client_accepted.
        void start() {
            this->conn_->set_handlers(
                [w = this->weak_from_this()](const uint8_t *p, std::size_t n) {
                    if (auto s = w.lock()) s->on_data(p, n);
                },
                [w = this->weak_from_this()](const std::error_code &ec) {
                    if (auto s = w.lock()) s->on_closed(ec);
                });
            this->conn_->start();
            if (handshake_timeout_.count() > 0) {
                auto t = std::make_shared<asio::steady_timer>(this->conn_->get_executor());
                t->expires_after(handshake_timeout_);
                t->async_wait([w = this->weak_from_this(), t](const std::error_code &ec) {
                    auto s = w.lock();
                    if (!ec && s && s->in_handshake()) s->conn_->abort(make_error_code(errc::timeout));
                });
            }
        }

    private:
        mutable std::mutex m_;
        std::shared_ptr<ws_settings> settings_;
        std::chrono::milliseconds handshake_timeout_;
        http_parser parser_;
        std::shared_ptr<detail::ws_engine> eng_;
        bool handshaking_ = true;
        http_request_t request_;

        bool in_handshake() const {
            std::lock_guard l(m_);
            return handshaking_;
        }

        std::shared_ptr<detail::ws_engine> engine() const {
            std::lock_guard l(m_);
            return eng_;
        }

        void on_data(const uint8_t *p, std::size_t n) {
            auto e = engine();
            if (e) { e->on_data(p, n); return; }
            if (!in_handshake()) return;
            std::error_code ec;
            const std::size_t used = parser_.feed(p, n, ec);
            if (ec) { reject(400, "Bad Request"); return; }
            if (!parser_.done()) return;
            http_request_t req = std::move(parser_.request());
            http_response_t res;
            if (!validate_handshake_request(req, res)) {
                on_unexpected_handshake(req);
                reject(res.body.find("Version") != std::string::npos ? 426 : 400, res.body);
                return;
            }
            res = {};
            res.status_code = 101;
            res.status_message = "Switching Protocols";
            res.headers["Upgrade"] = "websocket";
            res.headers["Connection"] = "Upgrade";
            res.headers["Sec-WebSocket-Accept"] = generate_accept_key(trim_whitespace(*find_header(req.headers, "sec-websocket-key")));
            if (!this->conn_->write(std::string_view(prepare_response(res)))) return;

            typename detail::ws_engine::hooks h;
            auto c = this->conn_;
            h.send = [c](std::vector<uint8_t> &&d, detail::ws_engine::write_cb cb) { return c->write(std::move(d), std::move(cb)); };
            h.close_transport = [c] { c->close(); };
            h.abort_transport = [c] { c->abort(); };
            h.on_message = [w = this->weak_from_this()](std::vector<uint8_t> &&m, bool bin) {
                if (auto s = w.lock()) s->on_message_received(m, bin);
            };
            h.on_ping = [w = this->weak_from_this()] { if (auto s = w.lock()) s->on_ping(); };
            h.on_pong = [w = this->weak_from_this()] { if (auto s = w.lock()) s->on_pong(); };
            auto engine = std::make_shared<detail::ws_engine>(asio::any_io_executor(c->get_executor()), false, std::move(h), settings_);
            {
                std::lock_guard l(m_);
                eng_ = engine;
                handshaking_ = false;
                request_ = req;
            }
            on_connected(req);
            engine->start_heartbeat();
            if (used < n) engine->on_data(p + used, n - used); // frames pipelined behind the handshake
        }

        void reject(int status, const std::string &msg) {
            http_response_t res;
            res.status_code = status;
            res.body = msg;
            res.headers["Connection"] = "close";
            res.headers["Content-Type"] = "text/plain";
            if (status == 426) res.headers["Sec-WebSocket-Version"] = "13";
            this->conn_->write(std::string_view(prepare_response(res)));
            this->conn_->close();
        }

        void on_closed(const std::error_code &ec) {
            auto e = engine();
            if (ec) { this->set_error(ec); on_error(ec); }
            if (e) {
                const auto st = e->on_transport_closed();
                on_close(st.first, st.second);
            }
            if (this->internal_on_closed) this->internal_on_closed();
        }
    };

    using ws_remote_c = basic_ws_remote<plain_stream>;
#ifdef IP_ENABLE_SSL
    using ws_remote_ssl_c = basic_ws_remote<ssl_stream>;
#endif
}
