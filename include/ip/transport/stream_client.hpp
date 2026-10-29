/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/connection.hpp"

namespace internetprotocol {
    /**
     * Client side connection manager shared by TCP, HTTP and WebSocket clients (plain or TLS):
     * async resolve -> async connect -> (TLS handshake) -> read loop, with connect timeout,
     * exponential-backoff reconnect and graceful close.
     *
     * The object is always owned through std::shared_ptr and every handler holds either a
     * shared_ptr or a weak_ptr to it, so a callback can never touch a destroyed client.
     */
    template<typename Stream>
    class stream_client : public std::enable_shared_from_this<stream_client<Stream>> {
    public:
        using conn_t = connection<Stream>;

        /// Transport established (and TLS handshake done, if any).
        event<void()> on_connected;
        /// Fired once after an ESTABLISHED connection ended (local close, peer close or error).
        event<void()> on_close;
        event<void(const std::error_code &)> on_error;

        /// Raw byte sink invoked on the connection strand. Must be set BEFORE connect().
        std::function<void(const uint8_t *, std::size_t)> data_hook;

        stream_client() = default;

        stream_client(const stream_client &) = delete;

        stream_client &operator=(const stream_client &) = delete;

#ifdef IP_ENABLE_SSL
        void set_tls(std::shared_ptr<asio::ssl::context> ctx, const security_context_opts &o, std::error_code ec) {
            ssl_ctx_ = std::move(ctx);
            tls_ec_ = ec;
            tls_host_ = o.host_name_verification;
            tls_verify_ = (o.verify_mode & verify_peer) != 0;
        }
#endif

        client_options_t &options() { return opts_; }

        bool is_open() const {
            std::lock_guard l(m_);
            return state_ == st::open;
        }

        bool is_connecting() const {
            std::lock_guard l(m_);
            return state_ == st::connecting;
        }

        std::error_code get_error_code() const {
            std::lock_guard l(m_);
            return ec_;
        }

        tcp::endpoint local_endpoint() const {
            auto c = connection_ptr();
            return c ? c->local_endpoint() : tcp::endpoint();
        }

        tcp::endpoint remote_endpoint() const {
            auto c = connection_ptr();
            return c ? c->remote_endpoint() : tcp::endpoint();
        }

        /// Starts an asynchronous connect. Returns false when already connecting/connected or the
        /// TLS configuration is invalid (reported through on_error).
        bool connect(const client_bind_options_t &b) {
            std::error_code cfg;
            {
                std::lock_guard l(m_);
                if (state_ != st::closed) return false;
#ifdef IP_ENABLE_SSL
                if constexpr (is_ssl_stream_v<Stream>) {
                    if (!ssl_ctx_) cfg = make_error_code(errc::tls_configuration);
                    else cfg = tls_ec_;
                }
#endif
                if (!cfg && b.address.empty()) cfg = make_error_code(errc::invalid_argument);
                if (cfg) ec_ = cfg;
                else {
                    state_ = st::connecting;
                    bind_ = b;
                    user_closed_ = false;
                    attempts_ = 0;
                }
            }
            if (cfg) {
                on_error(cfg);
                return false;
            }
            launch();
            return true;
        }

        bool write(std::vector<uint8_t> &&data, typename conn_t::write_callback cb = nullptr) {
            auto c = open_connection();
            return c && c->write(std::move(data), std::move(cb));
        }

        bool write(const uint8_t *data, std::size_t n, typename conn_t::write_callback cb = nullptr) {
            auto c = open_connection();
            return c && c->write(data, n, std::move(cb));
        }

        /// Graceful close: queued data is flushed first. Cancels reconnection.
        void close() { terminate(false); }

        /// Closes only the CURRENT connection (reconnection policy stays active). Used by protocol layers.
        void disconnect(bool hard = false) {
            auto c = connection_ptr();
            if (!c) return;
            if (hard) c->abort();
            else c->close();
        }

        /// Cancels any pending / future automatic reconnection without touching the connection.
        void cancel_reconnect() {
            std::shared_ptr<asio::steady_timer> t;
            {
                std::lock_guard l(m_);
                user_closed_ = true;
                t = std::move(reconnect_timer_);
            }
            if (t) t->cancel();
        }

        /// Executor (strand) of the current connection; empty if none.
        std::optional<asio::any_io_executor> executor() const {
            auto c = connection_ptr();
            if (!c) return std::nullopt;
            return asio::any_io_executor(c->get_executor());
        }

        /// Immediate close.
        void abort() { terminate(true); }

        /// Detaches every callback and aborts. Called by the owning facade's destructor.
        void shutdown() {
            on_connected.reset();
            on_close.reset();
            on_error.reset();
            terminate(true);
        }

    private:
        enum class st { closed, connecting, open, closing };

        struct attempt {
            explicit attempt(const strand_t &s) : resolver(s), deadline(s) {}

            tcp::resolver resolver;
            asio::steady_timer deadline;
            bool timed_out = false;
        };

        mutable std::mutex m_;
        st state_ = st::closed;
        client_options_t opts_;
        client_bind_options_t bind_;
        std::error_code ec_;
        std::shared_ptr<conn_t> conn_;
        std::shared_ptr<asio::steady_timer> reconnect_timer_;
        bool user_closed_ = false;
        unsigned attempts_ = 0;
#ifdef IP_ENABLE_SSL
        std::shared_ptr<asio::ssl::context> ssl_ctx_;
        std::error_code tls_ec_;
        std::string tls_host_;
        bool tls_verify_ = true;
#endif

        std::shared_ptr<conn_t> connection_ptr() const {
            std::lock_guard l(m_);
            return conn_;
        }

        std::shared_ptr<conn_t> open_connection() const {
            std::lock_guard l(m_);
            return state_ == st::open ? conn_ : nullptr;
        }

        bool is_current(const std::shared_ptr<conn_t> &c) const {
            std::lock_guard l(m_);
            return conn_ == c && !user_closed_;
        }

        Stream make_stream(const strand_t &s) {
            if constexpr (is_ssl_stream_v<Stream>) {
#ifdef IP_ENABLE_SSL
                return Stream(s, *ssl_ctx_);
#endif
            } else {
                return Stream(s);
            }
        }

        void terminate(bool hard) {
            std::shared_ptr<conn_t> c;
            std::shared_ptr<asio::steady_timer> t;
            bool was_open;
            {
                std::lock_guard l(m_);
                user_closed_ = true;
                t = std::move(reconnect_timer_);
                c = conn_;
                was_open = state_ == st::open;
                if (!was_open || hard) {
                    conn_.reset();
                    state_ = st::closed;
                } else {
                    state_ = st::closing;
                }
            }
            if (t) t->cancel();
            if (!c) return;
            if (hard) c->abort();
            else if (was_open) c->close();
            else c->discard();
        }

        void launch() {
            client_options_t o;
            client_bind_options_t b;
            {
                std::lock_guard l(m_);
                o = opts_;
                b = bind_;
            }
            connection_options co;
            co.read_buffer_size = o.read_buffer_size;
            co.max_write_queue_bytes = o.max_write_queue_bytes;
            co.idle_timeout = o.idle_timeout;
            co.tcp_no_delay = o.tcp_no_delay;
            co.keep_alive = o.keep_alive;

            const strand_t strand = make_runtime_strand();
            auto conn = std::make_shared<conn_t>(make_stream(strand), co);
            {
                std::lock_guard l(m_);
                if (user_closed_) return;
                conn_ = conn;
            }
            auto att = std::make_shared<attempt>(strand);
            auto self = this->shared_from_this();
            asio::post(strand, [self, conn, att, b, o] { self->begin(conn, att, b, o); });
        }

        void begin(const std::shared_ptr<conn_t> &conn, const std::shared_ptr<attempt> &att,
                   const client_bind_options_t &b, const client_options_t &o) {
            if (!is_current(conn)) return;
            auto self = this->shared_from_this();
            if (o.connect_timeout.count() > 0) {
                att->deadline.expires_after(o.connect_timeout);
                att->deadline.async_wait([conn, att](const std::error_code &ec) {
                    if (ec) return;
                    att->timed_out = true;
                    std::error_code ig;
                    att->resolver.cancel();
                    conn->stream().lowest_layer().close(ig);
                });
            }
            att->resolver.async_resolve(b.address, b.port,
                                        [self, conn, att, b](const std::error_code &ec, tcp::resolver::results_type results) {
                                            self->on_resolved(conn, att, b, ec, results);
                                        });
        }

        void on_resolved(const std::shared_ptr<conn_t> &conn, const std::shared_ptr<attempt> &att,
                         const client_bind_options_t &b, const std::error_code &ec,
                         const tcp::resolver::results_type &results) {
            if (!is_current(conn)) { att->deadline.cancel(); conn->discard(); return; }
            if (ec) { fail(conn, att, ec); return; }
            std::vector<tcp::endpoint> eps;
            for (const auto &r: results) {
                const tcp::endpoint e = r.endpoint();
                if (b.protocol == v4 && !e.address().is_v4()) continue;
                if (b.protocol == v6 && !e.address().is_v6()) continue;
                eps.push_back(e);
            }
            if (eps.empty()) { fail(conn, att, make_error_code(errc::host_not_found)); return; }
            auto self = this->shared_from_this();
            asio::async_connect(conn->stream().lowest_layer(), eps,
                                [self, conn, att, b](const std::error_code &e, const tcp::endpoint &) {
                                    self->on_tcp_connected(conn, att, b, e);
                                });
        }

        void on_tcp_connected(const std::shared_ptr<conn_t> &conn, const std::shared_ptr<attempt> &att,
                              const client_bind_options_t &b, const std::error_code &ec) {
            if (!is_current(conn)) { att->deadline.cancel(); conn->discard(); return; }
            if (ec) { fail(conn, att, ec); return; }
            if constexpr (is_ssl_stream_v<Stream>) {
#ifdef IP_ENABLE_SSL
                const std::string host = tls_host_.empty() ? b.address : tls_host_;
                SSL_set_tlsext_host_name(conn->stream().native_handle(), host.c_str());
                std::error_code vec;
                if (tls_verify_) conn->stream().set_verify_callback(asio::ssl::host_name_verification(host), vec);
                auto self = this->shared_from_this();
                conn->stream().async_handshake(asio::ssl::stream_base::client,
                                               [self, conn, att](const std::error_code &e) {
                                                   if (e) self->fail(conn, att, e);
                                                   else self->ready(conn, att);
                                               });
#endif
            } else {
                (void) b;
                ready(conn, att);
            }
        }

        void ready(const std::shared_ptr<conn_t> &conn, const std::shared_ptr<attempt> &att) {
            att->deadline.cancel();
            {
                std::lock_guard l(m_);
                if (conn_ != conn || user_closed_) { conn->discard(); return; }
                state_ = st::open;
                attempts_ = 0;
            }
            conn->set_handlers(data_hook ? data_hook : typename conn_t::data_callback{},
                               [w = this->weak_from_this(), raw = conn.get()](const std::error_code &ec) {
                                   if (auto s = w.lock()) s->on_conn_closed(raw, ec);
                               });
            conn->start();
            on_connected();
        }

        void on_conn_closed(const conn_t *raw, const std::error_code &ec) {
            bool reconnect = false;
            {
                std::lock_guard l(m_);
                if (conn_.get() != raw) return;
                conn_.reset();
                state_ = st::closed;
                if (ec) ec_ = ec;
                reconnect = reconnect_allowed_locked();
                if (reconnect) state_ = st::connecting;
            }
            if (ec) on_error(ec);
            on_close();
            if (reconnect) schedule_reconnect();
        }

        void fail(const std::shared_ptr<conn_t> &conn, const std::shared_ptr<attempt> &att, std::error_code ec) {
            att->deadline.cancel();
            if (att->timed_out) ec = make_error_code(errc::timeout);
            bool current, reconnect = false;
            {
                std::lock_guard l(m_);
                current = conn_ == conn && !user_closed_;
                if (current) {
                    conn_.reset();
                    state_ = st::closed;
                    ec_ = ec;
                    reconnect = reconnect_allowed_locked();
                    if (reconnect) state_ = st::connecting;
                }
            }
            conn->discard();
            if (!current) return;
            on_error(ec);
            if (reconnect) schedule_reconnect();
        }

        bool reconnect_allowed_locked() const {
            return opts_.reconnect.enabled && !user_closed_ &&
                   (opts_.reconnect.max_attempts == 0 || attempts_ < opts_.reconnect.max_attempts);
        }

        void schedule_reconnect() {
            milliseconds delay;
            {
                std::lock_guard l(m_);
                double d = static_cast<double>(opts_.reconnect.initial_delay.count());
                for (unsigned i = 0; i < attempts_ && d < 1e9; ++i) d *= std::max(1.0, opts_.reconnect.backoff_factor);
                delay = std::min(milliseconds(static_cast<long long>(d)), opts_.reconnect.max_delay);
                ++attempts_;
            }
            auto t = std::make_shared<asio::steady_timer>(runtime::instance().context());
            {
                std::lock_guard l(m_);
                if (user_closed_) return;
                reconnect_timer_ = t;
            }
            t->expires_after(delay);
            t->async_wait([self = this->shared_from_this(), t](const std::error_code &ec) {
                if (ec) return;
                {
                    std::lock_guard l(self->m_);
                    if (self->user_closed_ || self->state_ != st::connecting) return;
                }
                self->launch();
            });
        }
    };
}
