/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/tcp/tcpremote.hpp"
#include "ip/transport/stream_server.hpp"
#include "ip/utils/tls.hpp"

namespace internetprotocol {
    /**
     * TCP server (plain or TLS). Every accepted client is a basic_tcp_remote handed to
     * `on_client_accepted`, where its callbacks must be configured before reading starts.
     */
    template<typename Stream>
    class basic_tcp_server {
    public:
        using remote_t = basic_tcp_remote<Stream>;
        using core_t = stream_server<Stream, remote_t>;

    protected:
        // Declared before the public references below: initialisation order matters.
        std::shared_ptr<core_t> core_;

    public:
        basic_tcp_server() : basic_tcp_server(make_plain_core()) {}

        basic_tcp_server(const basic_tcp_server &) = delete;

        basic_tcp_server &operator=(const basic_tcp_server &) = delete;

        ~basic_tcp_server() { core_->shutdown(); }

        bool is_open() const { return core_->is_open(); }

        tcp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        /// Snapshot of the connected clients (safe to iterate while clients connect/disconnect).
        std::vector<std::shared_ptr<remote_t>> clients() const { return core_->clients(); }

        asio::error_code get_error_code() const { return core_->get_error_code(); }

        server_options_t &options() { return core_->options(); }

        /// Starts listening. Returns false (and fires on_error) if the socket cannot be bound.
        bool open(const server_bind_options_t &bind_opts = {"", 8080, v4, true}) { return core_->open(bind_opts); }

        /// Stops listening and closes all clients.
        void close() { core_->close(); }

        int &backlog;
        event<void()> &on_listening;
        event<void(const std::shared_ptr<remote_t> &)> &on_client_accepted;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    protected:
        explicit basic_tcp_server(std::shared_ptr<core_t> core)
            : core_(std::move(core)), backlog(core_->backlog), on_listening(core_->on_listening),
              on_client_accepted(core_->on_client_accepted), on_close(core_->on_close), on_error(core_->on_error) {}

        static std::shared_ptr<core_t> make_plain_core() {
            return std::make_shared<core_t>([](std::shared_ptr<connection<Stream>> c) {
                return std::make_shared<remote_t>(std::move(c));
            });
        }
    };

    using tcp_server_c = basic_tcp_server<plain_stream>;

#ifdef IP_ENABLE_SSL
    class tcp_server_ssl_c : public basic_tcp_server<ssl_stream> {
    public:
        explicit tcp_server_ssl_c(const security_context_opts &sec_opts = {})
            : basic_tcp_server<ssl_stream>(make_core(sec_opts)) {}

    private:
        static std::shared_ptr<core_t> make_core(const security_context_opts &o) {
            auto core = make_plain_core();
            std::error_code ec;
            auto ctx = make_server_ssl_context(o, ec);
            core->set_tls(std::move(ctx), ec);
            return core;
        }
    };
#endif
}
