/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/stream_server.hpp"
#include "ip/utils/tls.hpp"
#include "ip/websocket/wsremote.hpp"

namespace internetprotocol {
    /**
     * WebSocket server (plain or TLS).
     *
     * @code
     * ws_server_c server;
     * server.on_client_accepted = [](const std::shared_ptr<ws_remote_c> &client) {
     *     client->on_message_received = [w = std::weak_ptr(client)](const std::vector<uint8_t> &b, bool bin) {
     *         if (auto c = w.lock()) c->write_buffer(b);
     *     };
     * };
     * server.open({"", 8080, v4, true});
     * @endcode
     */
    template<typename Stream>
    class basic_ws_server {
    public:
        using remote_t = basic_ws_remote<Stream>;
        using core_t = stream_server<Stream, remote_t>;

    protected:
        // Declared before the public references below: initialisation order matters.
        std::shared_ptr<ws_settings> settings_ = std::make_shared<ws_settings>();
        std::shared_ptr<core_t> core_;

    public:
        basic_ws_server() : basic_ws_server(nullptr) {}

        basic_ws_server(const basic_ws_server &) = delete;

        basic_ws_server &operator=(const basic_ws_server &) = delete;

        ~basic_ws_server() { core_->shutdown(); }

        bool is_open() const { return core_->is_open(); }

        tcp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        std::vector<std::shared_ptr<remote_t>> clients() const { return core_->clients(); }

        asio::error_code get_error_code() const { return core_->get_error_code(); }

        server_options_t &options() { return core_->options(); }

        /// Applies to all connections (existing and new).
        void set_max_message_size(std::size_t bytes) { settings_->max_message_bytes.store(bytes); }

        void set_ping_interval(uint16_t seconds) { settings_->ping_interval_seconds.store(seconds); }

        bool open(const server_bind_options_t &bind_opts = {"", 8080, v4, true}) { return core_->open(bind_opts); }

        /// Stops listening and closes every client.
        void close() { core_->close(); }

        int &backlog;
        event<void()> &on_listening;
        event<void(const std::shared_ptr<remote_t> &)> &on_client_accepted;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    protected:
        explicit basic_ws_server(std::nullptr_t)
            : core_(make_core(settings_)), backlog(core_->backlog), on_listening(core_->on_listening),
              on_client_accepted(core_->on_client_accepted), on_close(core_->on_close), on_error(core_->on_error) {}

        explicit basic_ws_server(std::shared_ptr<ws_settings> s, std::shared_ptr<core_t> core)
            : settings_(std::move(s)), core_(std::move(core)), backlog(core_->backlog), on_listening(core_->on_listening),
              on_client_accepted(core_->on_client_accepted), on_close(core_->on_close), on_error(core_->on_error) {}

        static std::shared_ptr<core_t> make_core(const std::shared_ptr<ws_settings> &settings) {
            return std::make_shared<core_t>([settings](std::shared_ptr<connection<Stream>> c) {
                return std::make_shared<remote_t>(std::move(c), settings);
            });
        }
    };

    using ws_server_c = basic_ws_server<plain_stream>;

#ifdef IP_ENABLE_SSL
    class ws_server_ssl_c : public basic_ws_server<ssl_stream> {
    public:
        explicit ws_server_ssl_c(const security_context_opts &sec_opts = {})
            : ws_server_ssl_c(std::make_shared<ws_settings>(), sec_opts) {}

    private:
        ws_server_ssl_c(std::shared_ptr<ws_settings> s, const security_context_opts &o)
            : basic_ws_server<ssl_stream>(s, make_core(s)) {
            std::error_code ec;
            auto ctx = make_server_ssl_context(o, ec);
            core_->set_tls(std::move(ctx), ec);
        }
    };
#endif
}
