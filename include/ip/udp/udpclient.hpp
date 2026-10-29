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
        /// Shared UDP socket engine: strand-serialised receive loop + owned send buffers.
        class udp_core : public std::enable_shared_from_this<udp_core> {
        public:
            using send_cb = std::function<void(const std::error_code &, std::size_t)>;

            udp_core() : strand_(asio::make_strand(runtime::instance().context())), socket_(strand_) {}

            event<void()> on_ready;
            event<void(const std::vector<uint8_t> &, const size_t, const udp::endpoint &)> on_message_received;
            event<void()> on_close;
            event<void(const std::error_code &)> on_error;

            std::size_t max_pending_bytes = 16u * 1024u * 1024u;

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

            /// Opens (and optionally binds / connects) the socket synchronously, then starts receiving.
            bool activate(udp::socket &&opened, const udp::endpoint &local, const udp::endpoint &remote) {
                runtime::instance().start();
                {
                    std::lock_guard l(m_);
                    if (open_.load()) return false;
                    socket_ = std::move(opened);
                    local_ = local;
                    remote_ = remote;
                    closing_ = false;
                }
                open_.store(true, std::memory_order_release);
                on_ready();
                asio::post(strand_, [self = shared_from_this()] { self->receive(); });
                return true;
            }

            udp::socket &socket() { return socket_; }

            asio::any_io_executor executor() { return strand_; }

            /// Sends one datagram. `to` empty => connected socket send.
            bool send(std::vector<uint8_t> &&data, std::optional<udp::endpoint> to, send_cb cb) {
                if (data.empty() || !is_open()) return false;
                const std::size_t n = data.size();
                if (pending_.fetch_add(n) + n > max_pending_bytes) {
                    pending_.fetch_sub(n);
                    return false;
                }
                auto payload = std::make_shared<std::vector<uint8_t>>(std::move(data));
                asio::post(strand_, [self = shared_from_this(), payload, to, cb = std::move(cb)]() mutable {
                    if (!self->socket_.is_open()) {
                        self->pending_.fetch_sub(payload->size());
                        if (cb) try { cb(asio::error::operation_aborted, 0); } catch (...) {}
                        return;
                    }
                    auto h = [self, payload, cb = std::move(cb)](const std::error_code &ec, std::size_t sent) {
                        self->pending_.fetch_sub(payload->size());
                        if (ec && ec != asio::error::operation_aborted) self->report(ec);
                        if (cb) try { cb(ec, sent); } catch (...) {}
                    };
                    if (to) self->socket_.async_send_to(asio::buffer(*payload), *to, std::move(h));
                    else self->socket_.async_send(asio::buffer(*payload), std::move(h));
                });
                return true;
            }

            void close() {
                if (!open_.exchange(false)) return;
                auto self = shared_from_this();
                auto fin = [self] {
                    std::error_code ig;
                    if (self->socket_.is_open()) {
                        self->socket_.cancel(ig);
                        self->socket_.close(ig);
                    }
                    self->on_close();
                };
                if (strand_.running_in_this_thread()) fin();
                else asio::post(strand_, fin);
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
            strand_t strand_;
            udp::socket socket_;
            mutable std::mutex m_;
            udp::endpoint local_, remote_, sender_;
            std::error_code ec_;
            std::atomic<bool> open_{false};
            std::atomic<std::size_t> pending_{0};
            std::atomic<std::size_t> recv_size_{16384};
            std::vector<uint8_t> rbuf_;
            bool closing_ = false;

            void receive() {
                if (!socket_.is_open()) return;
                rbuf_.resize(recv_size_.load());
                socket_.async_receive_from(asio::buffer(rbuf_), sender_,
                                           [self = shared_from_this()](const std::error_code &ec, std::size_t n) {
                                               if (ec == asio::error::operation_aborted || !self->is_open()) return;
                                               if (ec) {
                                                   // ICMP "port unreachable" on a connected socket is transient for UDP.
                                                   self->report(ec);
                                                   if (!self->socket_.is_open()) return;
                                               } else {
                                                   self->on_message_received(self->rbuf_, n, self->sender_);
                                               }
                                               self->receive();
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
        std::shared_ptr<std::atomic<bool>> connecting_ = std::make_shared<std::atomic<bool>>(false);

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

        /// Allow sending to broadcast addresses (e.g. 255.255.255.255). Call after connect().
        bool set_broadcast(bool enable) {
            std::error_code ec;
            core_->socket().set_option(asio::socket_base::broadcast(enable), ec);
            return !ec;
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

        /// Resolves and connects asynchronously. 'on_connected' fires when ready.
        bool connect(const client_bind_options_t &bind_opts = {}) {
            if (core_->is_open() || connecting_->exchange(true)) return false;
            if (bind_opts.address.empty()) {
                connecting_->store(false);
                core_->report(make_error_code(errc::invalid_argument));
                return false;
            }
            runtime::instance().start();
            auto resolver = std::make_shared<udp::resolver>(runtime::instance().context());
            resolver->async_resolve(bind_opts.address, bind_opts.port,
                                    [core = core_, resolver, bind_opts, flag = connecting_](
                                const std::error_code &ec, udp::resolver::results_type results) {
                                        auto done = [&] { flag->store(false); };
                                        if (ec) { core->report(ec); done(); return; }
                                        for (const auto &r: results) {
                                            const udp::endpoint ep = r.endpoint();
                                            if (bind_opts.protocol == v4 && !ep.address().is_v4()) continue;
                                            if (bind_opts.protocol == v6 && !ep.address().is_v6()) continue;
                                            std::error_code e;
                                            udp::socket s(core->executor());
                                            s.open(ep.protocol(), e);
                                            if (!e) s.connect(ep, e);
                                            if (e) continue;
                                            const udp::endpoint local = s.local_endpoint(e);
                                            core->activate(std::move(s), local, ep);
                                            done();
                                            return;
                                        }
                                        core->report(make_error_code(errc::host_not_found));
                                        done();
                                    });
            return true;
        }

        void close() { core_->close(); }

        event<void()> &on_connected;
        /// buffer holds at least `bytes_recvd` valid bytes (the buffer is reused between datagrams).
        event<void(const std::vector<uint8_t> &, const size_t, const udp::endpoint &)> &on_message_received;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    };
}
