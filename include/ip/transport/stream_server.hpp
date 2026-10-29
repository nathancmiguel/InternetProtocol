/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/connection.hpp"

namespace internetprotocol {
    /**
     * Generic acceptor shared by TCP / HTTP / WebSocket servers (plain or TLS).
     * `Remote` must provide: Remote(std::shared_ptr<connection<Stream>>), void start(), void close(),
     * and a public `std::function<void()> internal_on_closed`.
     *
     * Every accepted connection gets its own strand; the accept loop runs on the server strand.
     * The client registry is mutex protected and exposed as snapshots (no iterator invalidation).
     */
    template<typename Stream, typename Remote>
    class stream_server : public std::enable_shared_from_this<stream_server<Stream, Remote>> {
    public:
        using conn_t = connection<Stream>;
        using remote_ptr = std::shared_ptr<Remote>;
        using remote_factory = std::function<remote_ptr(std::shared_ptr<conn_t>)>;

        event<void()> on_listening;
        event<void(const remote_ptr &)> on_client_accepted;
        event<void()> on_close;
        event<void(const std::error_code &)> on_error;

        int backlog = asio::socket_base::max_listen_connections;

        explicit stream_server(remote_factory factory)
            : factory_(std::move(factory)), strand_(asio::make_strand(runtime::instance().context())),
              acceptor_(strand_) {}

        stream_server(const stream_server &) = delete;

        stream_server &operator=(const stream_server &) = delete;

        virtual ~stream_server() = default;

#ifdef IP_ENABLE_SSL
        void set_tls(std::shared_ptr<asio::ssl::context> ctx, std::error_code ec) {
            ssl_ctx_ = std::move(ctx);
            tls_ec_ = ec;
        }
#endif

        server_options_t &options() { return opts_; }

        bool is_open() const { return listening_.load(std::memory_order_acquire); }

        tcp::endpoint local_endpoint() const {
            std::lock_guard l(m_);
            return local_;
        }

        std::error_code get_error_code() const {
            std::lock_guard l(m_);
            return ec_;
        }

        std::vector<remote_ptr> clients() const {
            std::lock_guard l(m_);
            std::vector<remote_ptr> v;
            v.reserve(clients_.size());
            for (const auto &kv: clients_) v.push_back(kv.second);
            return v;
        }

        std::size_t client_count() const {
            std::lock_guard l(m_);
            return clients_.size();
        }

        bool open(const server_bind_options_t &b) {
            std::error_code ec;
            {
                std::lock_guard l(m_);
                if (listening_.load()) return false;
#ifdef IP_ENABLE_SSL
                if constexpr (is_ssl_stream_v<Stream>) {
                    if (!ssl_ctx_) ec = make_error_code(errc::tls_configuration);
                    else ec = tls_ec_;
                }
#endif
            }
            if (ec) return report(ec);
            runtime::instance().start();

            const auto proto = b.protocol == v6 ? tcp::v6() : tcp::v4();
            acceptor_.open(proto, ec);
            if (ec) return report(ec);
            acceptor_.set_option(asio::socket_base::reuse_address(b.reuse_address), ec);
            if (ec) return fail_open(ec);
            tcp::endpoint ep(proto, b.port);
            if (!b.address.empty()) {
                const auto addr = make_address(b.address, ec);
                if (ec) return fail_open(ec);
                ep = tcp::endpoint(addr, b.port);
            }
            acceptor_.bind(ep, ec);
            if (ec) return fail_open(ec);
            acceptor_.listen(backlog, ec);
            if (ec) return fail_open(ec);
            {
                std::lock_guard l(m_);
                local_ = acceptor_.local_endpoint(ec);
                ec_.clear();
            }
            listening_.store(true, std::memory_order_release);
            on_listening();
            auto self = this->shared_from_this();
            asio::post(strand_, [self] { self->do_accept(); });
            return true;
        }

        /// Stops listening and gracefully closes every client. Blocks (briefly) until the acceptor is closed.
        void close() {
            std::vector<remote_ptr> rs;
            {
                std::lock_guard l(m_);
                if (!listening_.exchange(false)) return;
                for (auto &kv: clients_) rs.push_back(kv.second);
                clients_.clear();
            }
            close_acceptor();
            // internal_on_closed is intentionally left untouched (it may be read by the remote's strand);
            // the registry is already empty so the later remove() is a harmless no-op.
            for (auto &r: rs) r->close();
            on_close();
        }

        /// Detaches callbacks and closes. Called by the owning facade's destructor.
        void shutdown() {
            on_listening.reset();
            on_client_accepted.reset();
            on_close.reset();
            on_error.reset();
            close();
        }

    protected:
        /// Hook to wire protocol specific behaviour before the user's on_client_accepted runs.
        virtual void prepare_remote(const remote_ptr &) {}

    private:
        remote_factory factory_;
        server_options_t opts_;
        strand_t strand_;
        tcp::acceptor acceptor_;
        mutable std::mutex m_;
        std::unordered_map<Remote *, remote_ptr> clients_;
        tcp::endpoint local_;
        std::error_code ec_;
        std::atomic<bool> listening_{false};
#ifdef IP_ENABLE_SSL
        std::shared_ptr<asio::ssl::context> ssl_ctx_;
        std::error_code tls_ec_;
#endif

        bool report(const std::error_code &ec) {
            {
                std::lock_guard l(m_);
                ec_ = ec;
            }
            on_error(ec);
            return false;
        }

        bool fail_open(const std::error_code &ec) {
            std::error_code ig;
            acceptor_.close(ig);
            return report(ec);
        }

        void close_acceptor() {
            std::error_code ig;
            if (strand_.running_in_this_thread()) {
                acceptor_.close(ig);
                return;
            }
            auto p = std::make_shared<std::promise<void>>();
            auto f = p->get_future();
            asio::post(strand_, [this, p] {
                std::error_code e;
                acceptor_.close(e);
                p->set_value();
            });
            if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) acceptor_.close(ig); // runtime stopped
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

        void do_accept() {
            if (!acceptor_.is_open()) return;
            connection_options co;
            co.read_buffer_size = opts_.read_buffer_size;
            co.max_write_queue_bytes = opts_.max_write_queue_bytes;
            co.idle_timeout = opts_.idle_timeout;
            co.tcp_no_delay = opts_.tcp_no_delay;
            co.keep_alive = opts_.keep_alive;
            const strand_t cs = asio::make_strand(runtime::instance().context());
            auto conn = std::make_shared<conn_t>(make_stream(cs), co);
            acceptor_.async_accept(conn->stream().lowest_layer(),
                                   [self = this->shared_from_this(), conn](const std::error_code &ec) {
                                       self->on_accept(conn, ec);
                                   });
        }

        void on_accept(const std::shared_ptr<conn_t> &conn, const std::error_code &ec) {
            if (!listening_.load()) { conn->discard(); return; }
            if (ec) {
                if (ec == asio::error::operation_aborted) return;
                report(ec);
                // Back off briefly (EMFILE/ENFILE would otherwise spin the accept loop).
                auto t = std::make_shared<asio::steady_timer>(strand_);
                t->expires_after(std::chrono::milliseconds(50));
                t->async_wait([self = this->shared_from_this(), t](const std::error_code &e) {
                    if (!e) self->do_accept();
                });
                return;
            }
            do_accept();
            if (opts_.max_connections && client_count() >= opts_.max_connections) {
                conn->discard();
                return;
            }
            asio::post(conn->get_executor(), [self = this->shared_from_this(), conn] { self->setup(conn); });
        }

        void setup(const std::shared_ptr<conn_t> &conn) {
            if constexpr (is_ssl_stream_v<Stream>) {
#ifdef IP_ENABLE_SSL
                auto timer = std::make_shared<asio::steady_timer>(conn->get_executor());
                if (opts_.handshake_timeout.count() > 0) {
                    timer->expires_after(opts_.handshake_timeout);
                    timer->async_wait([conn](const std::error_code &e) {
                        if (!e) conn->abort(make_error_code(errc::timeout));
                    });
                }
                auto self = this->shared_from_this();
                conn->stream().async_handshake(asio::ssl::stream_base::server,
                                               [self, conn, timer](const std::error_code &e) {
                                                   timer->cancel();
                                                   if (e) {
                                                       conn->discard();
                                                       self->report(e);
                                                       return;
                                                   }
                                                   self->activate(conn);
                                               });
#endif
            } else {
                activate(conn);
            }
        }

        void activate(const std::shared_ptr<conn_t> &conn) {
            remote_ptr remote = factory_(conn);
            {
                std::lock_guard l(m_);
                if (!listening_.load()) { conn->discard(); return; }
                clients_.emplace(remote.get(), remote);
            }
            remote->internal_on_closed = [w = this->weak_from_this(), raw = remote.get()] {
                if (auto s = w.lock()) s->remove(raw);
            };
            prepare_remote(remote);
            on_client_accepted(remote);
            remote->start();
        }

        void remove(Remote *raw) {
            remote_ptr keep; // destroy outside the lock
            std::lock_guard l(m_);
            auto it = clients_.find(raw);
            if (it == clients_.end()) return;
            keep = std::move(it->second);
            clients_.erase(it);
        }
    };
}
