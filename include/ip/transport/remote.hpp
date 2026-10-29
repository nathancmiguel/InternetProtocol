/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/connection.hpp"

namespace internetprotocol {
    /// Shared base of every server-side connection (TCP / HTTP / WebSocket remotes).
    template<typename Stream>
    class remote_core {
    public:
        using conn_t = connection<Stream>;
        using write_callback = std::function<void(const std::error_code &, std::size_t)>;

        explicit remote_core(std::shared_ptr<conn_t> c) : conn_(std::move(c)) {}

        remote_core(const remote_core &) = delete;

        remote_core &operator=(const remote_core &) = delete;

        virtual ~remote_core() {
            if (conn_) conn_->abort();
        }

        /// Internal: used by the owning server to drop the client from its registry.
        std::function<void()> internal_on_closed;

        bool is_open() const noexcept { return conn_->is_open(); }

        tcp::endpoint local_endpoint() const { return conn_->local_endpoint(); }

        tcp::endpoint remote_endpoint() const { return conn_->remote_endpoint(); }

        std::error_code get_error_code() const {
            std::lock_guard l(ec_m_);
            return ec_;
        }

        /// Graceful close (queued data is flushed first).
        void close() { conn_->close(); }

        /// Immediate close.
        void abort() { conn_->abort(); }

        const std::shared_ptr<conn_t> &native_connection() const noexcept { return conn_; }

    protected:
        std::shared_ptr<conn_t> conn_;
        mutable std::mutex ec_m_;
        std::error_code ec_;

        void set_error(const std::error_code &ec) {
            std::lock_guard l(ec_m_);
            ec_ = ec;
        }
    };
}
