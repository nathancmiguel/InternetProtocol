/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/stream_client.hpp"
#include "ip/utils/httpparser.hpp"
#include "ip/utils/tls.hpp"
#include "ip/websocket/wsengine.hpp"

namespace internetprotocol {
    namespace detail {
        template<typename Stream>
        class ws_client_state : public std::enable_shared_from_this<ws_client_state<Stream>> {
        public:
            using write_cb = std::function<void(const std::error_code &, std::size_t)>;

            std::shared_ptr<stream_client<Stream>> link = std::make_shared<stream_client<Stream>>();
            std::shared_ptr<ws_settings> settings = std::make_shared<ws_settings>();

            event<void(const http_response_t &)> on_connected;
            event<void(const http_response_t &)> on_unexpected_handshake;
            event<void(const std::vector<uint8_t> &, bool)> on_message_received;
            event<void()> on_ping;
            event<void()> on_pong;
            event<void(const uint16_t, const std::string &)> on_close;
            event<void(const std::error_code &)> on_error;

            ws_client_state() : resp_parser_(http_parser::kind::response) {}

            void init() {
                auto w = this->weak_from_this();
                link->data_hook = [w](const uint8_t *p, std::size_t n) { if (auto s = w.lock()) s->on_data(p, n); };
                link->on_connected = [w] { if (auto s = w.lock()) s->send_handshake(); };
                link->on_close = [w] { if (auto s = w.lock()) s->on_link_closed(); };
                link->on_error = [w](const std::error_code &ec) { if (auto s = w.lock()) s->on_error(ec); };
            }

            void set_handshake_request(const http_request_t &r) {
                std::lock_guard l(m_);
                hs_req_ = r;
            }

            std::shared_ptr<ws_engine> engine() const {
                std::lock_guard l(m_);
                return eng_;
            }

            bool connect(const client_bind_options_t &b) {
                {
                    std::lock_guard l(m_);
                    bind_ = b;
                }
                return link->connect(b);
            }

            void shutdown() {
                on_connected.reset();
                on_unexpected_handshake.reset();
                on_message_received.reset();
                on_ping.reset();
                on_pong.reset();
                on_close.reset();
                on_error.reset();
                link->shutdown();
            }

        private:
            mutable std::mutex m_;
            client_bind_options_t bind_;
            http_request_t hs_req_;
            http_request_t sent_;
            http_parser resp_parser_;
            std::shared_ptr<ws_engine> eng_;
            bool handshaking_ = false;

            void send_handshake() {
                http_request_t req;
                client_bind_options_t b;
                {
                    std::lock_guard l(m_);
                    req = hs_req_;
                    b = bind_;
                    resp_parser_.reset();
                    eng_.reset();
                    handshaking_ = true;
                }
                req.method = GET;
                if (req.path.empty()) req.path = "/";
                req.headers["Upgrade"] = "websocket";
                req.headers["Connection"] = "Upgrade";
                req.headers["Sec-WebSocket-Key"] = generate_websocket_key(); // fresh nonce per connection
                req.headers["Sec-WebSocket-Version"] = "13";
                uint16_t port = 0;
                try { port = static_cast<uint16_t>(std::stoul(b.port)); } catch (...) {}
                const std::string payload = prepare_request(req, b.address, port);
                {
                    std::lock_guard l(m_);
                    sent_ = req;
                }
                if (!link->write(reinterpret_cast<const uint8_t *>(payload.data()), payload.size())) {
                    on_error(make_error_code(errc::not_connected));
                    link->disconnect(true);
                }
            }

            void on_data(const uint8_t *p, std::size_t n) {
                if (auto e = engine()) { e->on_data(p, n); return; }
                std::error_code ec;
                http_response_t res;
                http_request_t sent;
                std::size_t used;
                {
                    std::lock_guard l(m_);
                    if (!handshaking_) return;
                    used = resp_parser_.feed(p, n, ec);
                    if (!ec && !resp_parser_.done()) return;
                    if (!ec) res = std::move(resp_parser_.response());
                    sent = sent_;
                }
                if (ec) {
                    on_error(ec);
                    link->disconnect(true);
                    return;
                }
                if (!validate_handshake_response(sent, res)) {
                    on_unexpected_handshake(res);
                    on_error(make_error_code(errc::handshake_failed));
                    link->disconnect(true);
                    return;
                }
                auto ex = link->executor();
                if (!ex) return;
                ws_engine::hooks h;
                auto lk = link;
                h.send = [lk](std::vector<uint8_t> &&d, ws_engine::write_cb cb) { return lk->write(std::move(d), std::move(cb)); };
                h.close_transport = [lk] { lk->disconnect(false); };
                h.abort_transport = [lk] { lk->disconnect(true); };
                auto w = this->weak_from_this();
                h.on_message = [w](std::vector<uint8_t> &&m, bool bin) { if (auto s = w.lock()) s->on_message_received(m, bin); };
                h.on_ping = [w] { if (auto s = w.lock()) s->on_ping(); };
                h.on_pong = [w] { if (auto s = w.lock()) s->on_pong(); };
                auto engine = std::make_shared<ws_engine>(*ex, true, std::move(h), settings);
                {
                    std::lock_guard l(m_);
                    eng_ = engine;
                    handshaking_ = false;
                }
                on_connected(res);
                engine->start_heartbeat();
                if (used < n) engine->on_data(p + used, n - used);
            }

            void on_link_closed() {
                std::shared_ptr<ws_engine> e;
                {
                    std::lock_guard l(m_);
                    e = std::move(eng_);
                    handshaking_ = false;
                }
                if (!e) return; // never reached OPEN: the error was already reported
                const auto st = e->on_transport_closed();
                on_close(st.first, st.second);
            }
        };
    }

    /**
     * WebSocket client (plain or TLS). Callbacks run on a worker thread, serialised per connection.
     *
     * @code
     * ws_client_c ws;
     * ws.on_connected = [&](const http_response_t &) { ws.write("hello"); };
     * ws.on_message_received = [](const std::vector<uint8_t> &data, bool binary) { ... };
     * ws.connect({"localhost", "8080", v4});
     * @endcode
     */
    template<typename Stream>
    class basic_ws_client {
    protected:
        // Declared first: the public event references below are bound to state_ (initialisation order).
        std::shared_ptr<detail::ws_client_state<Stream>> state_;

    public:
        using write_callback = std::function<void(const asio::error_code &, const size_t)>;

        basic_ws_client() : basic_ws_client(std::make_shared<detail::ws_client_state<Stream>>()) {}

        basic_ws_client(const basic_ws_client &) = delete;

        basic_ws_client &operator=(const basic_ws_client &) = delete;

        ~basic_ws_client() { state_->shutdown(); }

        bool is_open() const {
            auto e = state_->engine();
            return e && e->is_open();
        }

        close_state_e get_state() const {
            auto e = state_->engine();
            return e ? e->state() : CLOSED;
        }

        asio::error_code get_error_code() const { return state_->link->get_error_code(); }

        tcp::endpoint local_endpoint() const { return state_->link->local_endpoint(); }

        tcp::endpoint remote_endpoint() const { return state_->link->remote_endpoint(); }

        /// Connect timeout, reconnection policy, idle timeout, buffer sizes. Configure before connect().
        client_options_t &options() { return state_->link->options(); }

        /// Request path / extra headers (e.g. Authorization, Origin, Sec-WebSocket-Protocol) for the upgrade.
        void set_handshake_request(const http_request_t &req) { state_->set_handshake_request(req); }

        void set_max_message_size(std::size_t bytes) { state_->settings->max_message_bytes.store(bytes); }

        /// Automatic keep-alive ping every `seconds` (0 = off). A silent peer is dropped after one more interval.
        void set_ping_interval(uint16_t seconds) { state_->settings->ping_interval_seconds.store(seconds); }

        bool write(const std::string &message, const dataframe_t &dataframe = {}, const write_callback &callback = nullptr) {
            auto e = state_->engine();
            return e && e->send_message(reinterpret_cast<const uint8_t *>(message.data()), message.size(), false, dataframe, callback);
        }

        bool write_buffer(const std::vector<uint8_t> &buffer, const dataframe_t &dataframe = {}, const write_callback &callback = nullptr) {
            auto e = state_->engine();
            return e && e->send_message(buffer.data(), buffer.size(), true, dataframe, callback);
        }

        bool ping(const write_callback &callback = nullptr) {
            auto e = state_->engine();
            return e && e->send_control(PING, {}, callback);
        }

        bool pong(const write_callback &callback = nullptr) {
            auto e = state_->engine();
            return e && e->send_control(PONG, {}, callback);
        }

        bool connect(const client_bind_options_t &bind_opts = {}) { return state_->connect(bind_opts); }

        /// Closing handshake: sends CLOSE and waits (bounded) for the server's CLOSE. Cancels reconnection.
        void end(const uint16_t code = 1000, const std::string &reason = "") {
            state_->link->cancel_reconnect();
            if (auto e = state_->engine()) e->close(code, reason, true);
            else state_->link->close();
        }

        /// Sends CLOSE, flushes and closes TCP without waiting. Cancels reconnection.
        void close(const uint16_t code = 1000, const std::string &reason = "") {
            state_->link->cancel_reconnect();
            if (auto e = state_->engine()) e->close(code, reason, false);
            else state_->link->close();
        }

        event<void(const http_response_t &)> &on_connected;
        event<void(const http_response_t &)> &on_unexpected_handshake;
        event<void(const std::vector<uint8_t> &, bool)> &on_message_received;
        event<void()> &on_ping;
        event<void()> &on_pong;
        event<void(const uint16_t, const std::string &)> &on_close;
        event<void(const asio::error_code &)> &on_error;

    protected:
        explicit basic_ws_client(std::shared_ptr<detail::ws_client_state<Stream>> st)
            : state_(std::move(st)), on_connected(state_->on_connected),
              on_unexpected_handshake(state_->on_unexpected_handshake),
              on_message_received(state_->on_message_received), on_ping(state_->on_ping),
              on_pong(state_->on_pong), on_close(state_->on_close), on_error(state_->on_error) {
            state_->init();
        }
    };

    using ws_client_c = basic_ws_client<plain_stream>;

#ifdef IP_ENABLE_SSL
    class ws_client_ssl_c : public basic_ws_client<ssl_stream> {
    public:
        explicit ws_client_ssl_c(const security_context_opts &sec_opts = {})
            : basic_ws_client<ssl_stream>(make_state(sec_opts)) {}

    private:
        static std::shared_ptr<detail::ws_client_state<ssl_stream>> make_state(const security_context_opts &o) {
            auto st = std::make_shared<detail::ws_client_state<ssl_stream>>();
            std::error_code ec;
            auto ctx = make_client_ssl_context(o, ec);
            st->link->set_tls(std::move(ctx), o, ec);
            return st;
        }
    };
#endif
}
