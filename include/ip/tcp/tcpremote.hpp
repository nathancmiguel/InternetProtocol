/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/remote.hpp"

namespace internetprotocol {
    /// A client connected to a tcp_server (plain or TLS).
    template<typename Stream>
    class basic_tcp_remote : public remote_core<Stream>, public std::enable_shared_from_this<basic_tcp_remote<Stream>> {
    public:
        using conn_t = connection<Stream>;
        using write_callback = std::function<void(const asio::error_code &, const size_t)>;

        explicit basic_tcp_remote(std::shared_ptr<conn_t> c) : remote_core<Stream>(std::move(c)) {}

        /// Sends text/bytes. Returns false if the connection is closed, the data is empty or the queue is full.
        bool write(const std::string &message, const write_callback &callback = nullptr) {
            return this->conn_->write(std::string_view(message), callback);
        }

        bool write_buffer(const std::vector<uint8_t> &buffer, const write_callback &callback = nullptr) {
            return this->conn_->write(buffer.data(), buffer.size(), callback);
        }

        bool write_buffer(std::vector<uint8_t> &&buffer, const write_callback &callback = nullptr) {
            return this->conn_->write(std::move(buffer), callback);
        }

        /// Called on the connection strand for every chunk of received bytes.
        event<void(const std::vector<uint8_t> &, const size_t)> on_message_received;
        event<void()> on_close;
        event<void(const asio::error_code &)> on_error;

        /// Internal: begins reading. Called by the server after on_client_accepted.
        void start() {
            this->conn_->set_handlers(
                [w = this->weak_from_this()](const uint8_t *p, std::size_t n) {
                    auto s = w.lock();
                    if (!s || !s->on_message_received) return;
                    s->on_message_received(std::vector<uint8_t>(p, p + n), n);
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
    };

    using tcp_remote_c = basic_tcp_remote<plain_stream>;
#ifdef IP_ENABLE_SSL
    using tcp_remote_ssl_c = basic_tcp_remote<ssl_stream>;
#endif
}
