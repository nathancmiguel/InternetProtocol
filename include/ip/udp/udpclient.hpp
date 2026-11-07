/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/runtime.hpp"
#include "ip/core/types.hpp"
#include "ip/events/event.hpp"

namespace internetprotocol {
    namespace detail {
        /**
         * Shared UDP socket engine: strand-serialised receive loop + owned send buffers.
         *
         * Every open()/bind()/connect() creates a new *session* (socket + receive buffer). Handlers capture
         * their own session, so a quick close() -> bind() can never make a stale handler close the new socket
         * or start a second receive loop on it.
         */
        class udp_core : public std::enable_shared_from_this<udp_core> {
        public:
            using send_cb = std::function<void(const std::error_code &, std::size_t)>;

            udp_core() : strand_(asio::make_strand(runtime::instance().context())) {}

            event<void()> on_ready;
            event<void(const std::vector<uint8_t> &, const size_t, const udp::endpoint &)> on_message_received;
            event<void()> on_close;
            event<void(const std::error_code &)> on_error;

            std::atomic<std::size_t> max_pending_bytes{16u * 1024u * 1024u};

            void set_recv_buffer_size(std::size_t n) { recv_size_.store(std::max<std::size_t>(n, 64)); }

            std::size_t recv_buffer_size() const { return recv_size_.load(); }

            bool is_open() const { return open_.load(std::memory_order_acquire); }

            udp::endpoint local_endpoint() const {
                std::lock_guard l(m_);
                return local_;
            }

            udp::endpoint remote_endpoint() const {
                std::lock_guard l(m_);
                return remote_;
            }

            std::error_code get_error_code() const {
                std::lock_guard l(m_);
                return ec_;
            }

            asio::any_io_executor executor() { return strand_; }

            /// Takes an opened (and optionally bound / connected) socket and starts receiving.
            bool activate(udp::socket &&opened, const udp::endpoint &local, const udp::endpoint &remote) {
                runtime::instance().start();
                std::shared_ptr<session> s;
                {
                    std::lock_guard l(m_);
                    if (open_.load()) return false;
                    s = std::make_shared<session>(std::move(opened));
                    cur_ = s;
                    local_ = local;
                    remote_ = remote;
                    open_.store(true, std::memory_order_release);
                }
                on_ready();
                asio::post(strand_, [self = shared_from_this(), s] { self->receive(s); });
                return true;
            }

            /// Asynchronous resolve + "connect" of a datagram socket. Cancelled by close().
            bool connect(const client_bind_options_t &b) {
                if (b.address.empty()) {
                    report(make_error_code(errc::invalid_argument));
                    return false;
                }
                runtime::instance().start();
                std::shared_ptr<udp::resolver> r;
                std::uint64_t seq;
                {
                    std::lock_guard l(m_);
                    if (open_.load() || resolver_) return false;
                    r = std::make_shared<udp::resolver>(strand_);
                    resolver_ = r;
                    seq = ++connect_seq_;
                }
                r->async_resolve(b.address, b.port,
                                 [self = shared_from_this(), r, b, seq](const std::error_code &ec, udp::resolver::results_type results) {
                                     self->on_resolved(seq, b, ec, results);
                                 });
                return true;
            }

            /// Runs `f(socket)` on the current socket (socket options). Error if not open.
            template<typename F>
            std::error_code with_socket(F &&f) {
                std::lock_guard l(m_);
                if (!cur_) return make_error_code(errc::not_connected);
                std::error_code ec;
                f(cur_->sock, ec);
                return ec;
            }

            /// Sends one datagram. `to` empty => connected socket send.
            bool send(std::vector<uint8_t> &&data, std::optional<udp::endpoint> to, send_cb cb) {
                if (data.empty()) return false;
                std::shared_ptr<session> s;
                {
                    std::lock_guard l(m_);
                    if (!open_.load()) return false;
                    s = cur_;
                }
                if (!s) return false;
                const std::size_t n = data.size();
                if (pending_.fetch_add(n) + n > max_pending_bytes.load()) {
                    pending_.fetch_sub(n);
                    return false;
                }
                auto payload = std::make_shared<std::vector<uint8_t>>(std::move(data));
                asio::post(strand_, [self = shared_from_this(), s, payload, to, cb = std::move(cb)]() mutable {
                    if (!s->sock.is_open()) {
                        self->pending_.fetch_sub(payload->size());
                        if (cb) try { cb(asio::error::operation_aborted, 0); } catch (...) {}
                        return;
                    }
                    auto h = [self, payload, cb = std::move(cb)](const std::error_code &ec, std::size_t sent) {
                        self->pending_.fetch_sub(payload->size());
                        if (ec && ec != asio::error::operation_aborted) self->report(ec);
                        if (cb) try { cb(ec, sent); } catch (...) {}
                    };
                    if (to) s->sock.async_send_to(asio::buffer(*payload), *to, std::move(h));
                    else s->sock.async_send(asio::buffer(*payload), std::move(h));
                });
                return true;
            }

            /// Closes the socket (fires on_close if it was open) and cancels a pending connect().
            void close() {
                std::shared_ptr<session> s;
                std::shared_ptr<udp::resolver> r;
                bool was_open;
                {
                    std::lock_guard l(m_);
                    was_open = open_.exchange(false);
                    s = std::move(cur_);
                    r = std::move(resolver_);
                    ++connect_seq_;
                }
                if (!was_open && !r) return;
                asio::dispatch(strand_, [self = shared_from_this(), s, r, was_open] {
                    std::error_code ig;
                    if (r) r->cancel();
                    if (s && s->sock.is_open()) {
                        s->sock.cancel(ig);
                        s->sock.close(ig);
                    }
                    if (was_open) self->on_close();
                });
            }

            void shutdown() {
                on_ready.reset();
                on_message_received.reset();
                on_close.reset();
                on_error.reset();
                close();
            }

            void report(const std::error_code &ec) {
                {
                    std::lock_guard l(m_);
                    ec_ = ec;
                }
                on_error(ec);
            }

        private:
            struct session {
                explicit session(udp::socket &&s) : sock(std::move(s)) {}

                udp::socket sock;
                std::vector<uint8_t> rbuf;
                udp::endpoint sender;
            };

            strand_t strand_;
            mutable std::mutex m_;
            std::shared_ptr<session> cur_;
            std::shared_ptr<udp::resolver> resolver_;
            std::uint64_t connect_seq_ = 0;
            udp::endpoint local_, remote_;
            std::error_code ec_;
            std::atomic<bool> open_{false};
            std::atomic<std::size_t> pending_{0};
            std::atomic<std::size_t> recv_size_{16384};

            bool is_current(const std::shared_ptr<session> &s) const {
                std::lock_guard l(m_);
                return cur_ == s;
            }

            void on_resolved(std::uint64_t seq, const client_bind_options_t &b, const std::error_code &ec,
                             const udp::resolver::results_type &results) {
                {
                    std::lock_guard l(m_);
                    if (seq != connect_seq_) return; // cancelled by close() (or superseded)
                    resolver_.reset();
                }
                if (ec) { report(ec); return; }
                for (const auto &r: results) {
                    const udp::endpoint ep = r.endpoint();
                    if (b.protocol == v4 && !ep.address().is_v4()) continue;
                    if (b.protocol == v6 && !ep.address().is_v6()) continue;
                    std::error_code e;
                    udp::socket s(strand_);
                    s.open(ep.protocol(), e);
                    if (!e) s.connect(ep, e);
                    if (e) continue;
                    const udp::endpoint local = s.local_endpoint(e);
                    activate(std::move(s), local, ep);
                    return;
                }
                report(make_error_code(errc::host_not_found));
            }

            void receive(const std::shared_ptr<session> &s) {
                if (!s->sock.is_open() || !is_current(s)) return;
                const std::size_t want = recv_size_.load();
                if (s->rbuf.size() != want) s->rbuf.resize(want);
                s->sock.async_receive_from(asio::buffer(s->rbuf), s->sender,
                                           [self = shared_from_this(), s](const std::error_code &ec, std::size_t n) {
                                               if (ec == asio::error::operation_aborted || !self->is_current(s)) return;
                                               if (ec) {
                                                   // ICMP "port unreachable" on a connected socket is transient for UDP.
                                                   self->report(ec);
                                                   if (!s->sock.is_open()) return;
                                               } else {
                                                   self->on_message_received(s->rbuf, n, s->sender);
                                               }
                                               self->receive(s);
                                           });
            }
        };
    }

    /**
     * UDP client: resolves the host, "connects" the datagram socket (fixed peer) and receives.
     * Unlike the old implementation the destination is never overwritten by the sender address.
     */
    class udp_client_c {
        // Declared first: the public event references below are bound to core_ (initialisation order).
        std::shared_ptr<detail::udp_core> core_ = std::make_shared<detail::udp_core>();

    public:
        using send_callback = std::function<void(const asio::error_code &, const size_t)>;

        udp_client_c() : on_connected(core_->on_ready), on_message_received(core_->on_message_received),
                         on_close(core_->on_close), on_error(core_->on_error) {}

        udp_client_c(const udp_client_c &) = delete;

        udp_client_c &operator=(const udp_client_c &) = delete;

        ~udp_client_c() { core_->shutdown(); }

        bool is_open() const { return core_->is_open(); }

        udp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        udp::endpoint remote_endpoint() const { return core_->remote_endpoint(); }

        asio::error_code get_error_code() const { return core_->get_error_code(); }

        /// Size of the receive buffer, i.e. the largest datagram delivered intact (default 16 KiB).
        void set_recv_buffer_size(const size_t size = 16384) { core_->set_recv_buffer_size(size); }

        size_t get_recv_buffer_size() const { return core_->recv_buffer_size(); }

        /// Allow sending to broadcast addresses (e.g. 255.255.255.255). Call after on_connected.
        bool set_broadcast(bool enable) {
            return !core_->with_socket([&](udp::socket &s, std::error_code &ec) {
                s.set_option(asio::socket_base::broadcast(enable), ec);
            });
        }

        bool send(const std::string &message, const send_callback &callback = nullptr) {
            return core_->send(std::vector<uint8_t>(message.begin(), message.end()), std::nullopt, callback);
        }

        bool send_buffer(const std::vector<uint8_t> &buffer, const send_callback &callback = nullptr) {
            return core_->send(std::vector<uint8_t>(buffer), std::nullopt, callback);
        }

        bool send_buffer(std::vector<uint8_t> &&buffer, const send_callback &callback = nullptr) {
            return core_->send(std::move(buffer), std::nullopt, callback);
        }

        /// Resolves and connects asynchronously. 'on_connected' fires when ready. close() cancels it.
        bool connect(const client_bind_options_t &bind_opts = {}) { return core_->connect(bind_opts); }

        void close() { core_->close(); }

        event<void()> &on_connected;
        /// buffer holds at least `bytes_recvd` valid bytes (the buffer is reused between datagrams).
        event<void(const std::vector<uint8_t> &, const size_t, const udp::endpoint &)> &on_message_received;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;
    };
}
