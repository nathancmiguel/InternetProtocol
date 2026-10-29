/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/udp/udpclient.hpp"

namespace internetprotocol {
    /// UDP server: bind, receive_from, send_to, broadcast and multicast.
    class udp_server_c {
        std::shared_ptr<detail::udp_core> core_ = std::make_shared<detail::udp_core>();

    public:
        udp_server_c() : on_listening(core_->on_ready), on_message_received(core_->on_message_received),
                         on_close(core_->on_close), on_error(core_->on_error) {}

        udp_server_c(const udp_server_c &) = delete;

        udp_server_c &operator=(const udp_server_c &) = delete;

        ~udp_server_c() { core_->shutdown(); }

        bool is_open() const { return core_->is_open(); }

        udp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        asio::error_code get_error_code() const { return core_->get_error_code(); }

        void set_recv_buffer_size(const size_t size = 16384) { core_->set_recv_buffer_size(size); }

        size_t get_recv_buffer_size() const { return core_->recv_buffer_size(); }

        /// Sends a datagram to `endpoint`. The callback also receives the destination endpoint.
        bool send_to(const std::string &message, const udp::endpoint &endpoint,
                     const std::function<void(const asio::error_code &, const size_t, const udp::endpoint &)> &callback = nullptr) {
            return core_->send(std::vector<uint8_t>(message.begin(), message.end()), endpoint,
                               callback ? detail::udp_core::send_cb([callback, endpoint](const std::error_code &ec, std::size_t n) {
                                   callback(ec, n, endpoint);
                               }) : detail::udp_core::send_cb{});
        }

        bool send_buffer_to(const std::vector<uint8_t> &buffer, const udp::endpoint &endpoint,
                            const std::function<void(const asio::error_code &, const size_t)> &callback = nullptr) {
            return core_->send(std::vector<uint8_t>(buffer), endpoint, callback);
        }

        bool send_buffer_to(std::vector<uint8_t> &&buffer, const udp::endpoint &endpoint,
                            const std::function<void(const asio::error_code &, const size_t)> &callback = nullptr) {
            return core_->send(std::move(buffer), endpoint, callback);
        }

        /// Enables sending to broadcast addresses such as `address_v4::broadcast()`.
        bool set_broadcast(bool enable) {
            std::error_code ec;
            core_->socket().set_option(asio::socket_base::broadcast(enable), ec);
            return !ec;
        }

        /// Joins a multicast group on the default interface (or on `interface_address`). Call after bind().
        bool join_multicast_group(const std::string &group, const std::string &interface_address = "") {
            return multicast(group, interface_address, true);
        }

        bool leave_multicast_group(const std::string &group, const std::string &interface_address = "") {
            return multicast(group, interface_address, false);
        }

        /// Multicast TTL / hop limit and local loopback of outgoing multicast datagrams.
        bool set_multicast_options(unsigned ttl = 1, bool loopback = true) {
            std::error_code ec;
            core_->socket().set_option(asio::ip::multicast::hops(ttl), ec);
            if (ec) { core_->report(ec); return false; }
            core_->socket().set_option(asio::ip::multicast::enable_loopback(loopback), ec);
            if (ec) { core_->report(ec); return false; }
            return true;
        }

        /// Opens and binds the socket synchronously, then starts receiving. Errors go to on_error.
        bool bind(const server_bind_options_t &bind_opts = {"", 8080, v4, true}) {
            if (core_->is_open()) return false;
            std::error_code ec;
            udp::socket s(core_->executor());
            const auto proto = bind_opts.protocol == v6 ? udp::v6() : udp::v4();
            s.open(proto, ec);
            if (ec) { core_->report(ec); return false; }
            s.set_option(asio::socket_base::reuse_address(bind_opts.reuse_address), ec);
            if (ec) { core_->report(ec); return false; }
            udp::endpoint ep(proto, bind_opts.port);
            if (!bind_opts.address.empty()) {
                const auto addr = make_address(bind_opts.address, ec);
                if (ec) { core_->report(ec); return false; }
                ep = udp::endpoint(addr, bind_opts.port);
            }
            s.bind(ep, ec);
            if (ec) { core_->report(ec); return false; }
            const udp::endpoint local = s.local_endpoint(ec);
            // Keep a reference socket for option changes before activation moves it.
            return core_->activate(std::move(s), local, udp::endpoint());
        }

        void close() { core_->close(); }

        event<void()> &on_listening;
        /// buffer holds at least `bytes_recvd` valid bytes (the buffer is reused between datagrams).
        event<void(const std::vector<uint8_t> &, const size_t, const udp::endpoint &)> &on_message_received;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    private:
        bool multicast(const std::string &group, const std::string &iface, bool join) {
            std::error_code ec;
            const auto addr = make_address(group, ec);
            if (ec) { core_->report(ec); return false; }
            if (!addr.is_multicast()) { core_->report(make_error_code(errc::invalid_argument)); return false; }
            if (addr.is_v4()) {
                const auto ifa = iface.empty() ? address_v4::any() : asio::ip::make_address_v4(iface, ec);
                if (ec) { core_->report(ec); return false; }
                if (join) core_->socket().set_option(asio::ip::multicast::join_group(addr.to_v4(), ifa), ec);
                else core_->socket().set_option(asio::ip::multicast::leave_group(addr.to_v4(), ifa), ec);
            } else {
                if (join) core_->socket().set_option(asio::ip::multicast::join_group(addr), ec);
                else core_->socket().set_option(asio::ip::multicast::leave_group(addr), ec);
            }
            if (ec) { core_->report(ec); return false; }
            return true;
        }
    };
}
