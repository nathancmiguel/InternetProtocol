/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/stream_client.hpp"
#include "ip/utils/tls.hpp"

namespace internetprotocol {
    /**
     * TCP client (plain or TLS). All callbacks run on a worker thread of the shared runtime,
     * serialised per connection. Methods are thread-safe.
     *
     * @code
     * tcp_client_c client;
     * client.options().reconnect.enabled = true;
     * client.on_connected = [] { ... };
     * client.on_message_received = [](const std::vector<uint8_t> &data, size_t n) { ... };
     * client.connect({"localhost", "8080", v4});
     * client.write("hello");
     * @endcode
     */
    template<typename Stream>
    class basic_tcp_client {
    protected:
        // NOTE: declared before the public event references below: initialisation order matters.
        std::shared_ptr<stream_client<Stream>> core_;

    private:
        using msg_event = event<void(const std::vector<uint8_t> &, const size_t)>;
        std::shared_ptr<msg_event> msg_ = std::make_shared<msg_event>();

    public:
        using write_callback = std::function<void(const asio::error_code &, const size_t)>;

        basic_tcp_client() : basic_tcp_client(std::make_shared<stream_client<Stream>>()) {}

        basic_tcp_client(const basic_tcp_client &) = delete;

        basic_tcp_client &operator=(const basic_tcp_client &) = delete;

        ~basic_tcp_client() { core_->shutdown(); }

        bool is_open() const { return core_->is_open(); }

        tcp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        tcp::endpoint remote_endpoint() const { return core_->remote_endpoint(); }

        /// Latest error reported by the connection (by value: thread safe).
        asio::error_code get_error_code() const { return core_->get_error_code(); }

        /// Timeouts, keep-alive, buffer sizes and reconnection policy. Configure before connect().
        client_options_t &options() { return core_->options(); }

        /// Queues text for sending. Returns false if closed, empty, or the queue limit is exceeded.
        bool write(const std::string &message, const write_callback &callback = nullptr) {
            return core_->write(reinterpret_cast<const uint8_t *>(message.data()), message.size(), wrap(callback));
        }

        bool write_buffer(const std::vector<uint8_t> &buffer, const write_callback &callback = nullptr) {
            return core_->write(buffer.data(), buffer.size(), wrap(callback));
        }

        bool write_buffer(std::vector<uint8_t> &&buffer, const write_callback &callback = nullptr) {
            return core_->write(std::move(buffer), wrap(callback));
        }

        /// Asynchronous connect. Returns false if already connecting/connected or the arguments are invalid.
        bool connect(const client_bind_options_t &bind_opts = {}) { return core_->connect(bind_opts); }

        /// Graceful close: pending writes are flushed, 'on_close' fires, reconnection is cancelled.
        void close() { core_->close(); }

        /// Immediate close, discarding pending writes.
        void abort() { core_->abort(); }

        event<void()> &on_connected;
        event<void(const std::vector<uint8_t> &, const size_t)> &on_message_received;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    protected:
        explicit basic_tcp_client(std::shared_ptr<stream_client<Stream>> core)
            : core_(std::move(core)),
              on_connected(core_->on_connected),
              on_message_received(*msg_),
              on_close(core_->on_close),
              on_error(core_->on_error) {
            core_->data_hook = [m = msg_](const uint8_t *p, std::size_t n) {
                if (*m) (*m)(std::vector<uint8_t>(p, p + n), n);
            };
        }

    private:
        static typename connection<Stream>::write_callback wrap(const write_callback &cb) {
            return cb ? typename connection<Stream>::write_callback(cb) : typename connection<Stream>::write_callback{};
        }
    };

    using tcp_client_c = basic_tcp_client<plain_stream>;

#ifdef IP_ENABLE_SSL
    class tcp_client_ssl_c : public basic_tcp_client<ssl_stream> {
    public:
        explicit tcp_client_ssl_c(const security_context_opts &sec_opts = {})
            : basic_tcp_client<ssl_stream>(make_core(sec_opts)) {}

    private:
        static std::shared_ptr<stream_client<ssl_stream>> make_core(const security_context_opts &o) {
            auto core = std::make_shared<stream_client<ssl_stream>>();
            std::error_code ec;
            auto ctx = make_client_ssl_context(o, ec);
            core->set_tls(std::move(ctx), o, ec);
            return core;
        }
    };
#endif
}
