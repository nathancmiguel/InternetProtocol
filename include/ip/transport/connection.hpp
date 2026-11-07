/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/runtime.hpp"
#include "ip/core/types.hpp"
#include "ip/events/event.hpp"

namespace internetprotocol {
    using plain_stream = tcp::socket;
#ifdef IP_ENABLE_SSL
    using ssl_stream = asio::ssl::stream<tcp::socket>;
#endif

    template<typename S>
    inline constexpr bool is_ssl_stream_v = false;
#ifdef IP_ENABLE_SSL
    template<typename S>
    inline constexpr bool is_ssl_stream_v<asio::ssl::stream<S>> = true;
#endif

    struct connection_options {
        std::size_t read_buffer_size = 16384;
        std::size_t max_write_queue_bytes = 64u * 1024u * 1024u;
        milliseconds idle_timeout{0};
        bool tcp_no_delay = true;
        bool keep_alive = true;
        /// Graceful close: max time spent waiting for the peer's FIN / close_notify before dropping the socket.
        milliseconds linger_timeout{3000};
    };

    /**
     * One established (or being established) byte stream with:
     *  - all handlers serialised on the stream's strand (no data races, no locks in the I/O path),
     *  - an owned, ordered, back-pressured write queue (buffers outlive the async operation),
     *  - idle timeout,
     *  - graceful close (flushes queued data, half-closes and waits for the peer) and immediate abort.
     *
     * Lifetime: every asynchronous handler captures shared_from_this(); the object can therefore
     * never be destroyed under a pending operation. Public methods are thread-safe.
     */
    template<typename Stream>
    class connection : public std::enable_shared_from_this<connection<Stream>> {
    public:
        using write_callback = std::function<void(const std::error_code &, std::size_t)>;
        using data_callback = std::function<void(const uint8_t *, std::size_t)>;
        using close_callback = std::function<void(const std::error_code &)>;

        connection(Stream &&stream, const connection_options &opts)
            : stream_(std::move(stream)), opts_(opts), timer_(stream_.get_executor()),
              rbuf_(std::max<std::size_t>(opts.read_buffer_size, 512)) {}

        connection(const connection &) = delete;

        connection &operator=(const connection &) = delete;

        /// Direct stream access. Only valid on the strand, before start().
        Stream &stream() noexcept { return stream_; }

        auto get_executor() { return stream_.get_executor(); }

        /// Handlers run on the strand. Set before start(); they are released on close (breaks cycles).
        void set_handlers(data_callback on_data, close_callback on_close) {
            on_data_ = std::move(on_data);
            on_close_ = std::move(on_close);
        }

        /// Begins reading. Must be called on the strand (or right after construction, before sharing).
        void start() {
            auto self = this->shared_from_this();
            asio::dispatch(stream_.get_executor(), [self] { self->do_start(); });
        }

        bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }

        tcp::endpoint local_endpoint() const {
            std::lock_guard l(ep_mutex_);
            return local_;
        }

        tcp::endpoint remote_endpoint() const {
            std::lock_guard l(ep_mutex_);
            return remote_;
        }

        /// Queues `data` (taking ownership). Returns false if closed or the queue limit is exceeded.
        bool write(std::vector<uint8_t> &&data, write_callback cb = nullptr) {
            pending p;
            p.vec = std::move(data);
            p.cb = std::move(cb);
            return push(std::move(p));
        }

        /// Queues a string without copying it (taking ownership).
        bool write_string(std::string &&data, write_callback cb = nullptr) {
            pending p;
            p.str = std::move(data);
            p.is_str = true;
            p.cb = std::move(cb);
            return push(std::move(p));
        }

        bool write(const uint8_t *data, std::size_t n, write_callback cb = nullptr) {
            if (n == 0) return false;
            return write(std::vector<uint8_t>(data, data + n), std::move(cb));
        }

        bool write(std::string_view s, write_callback cb = nullptr) {
            if (s.empty()) return false;
            return write_string(std::string(s), std::move(cb));
        }

        /// Graceful: flushes everything already queued, then closes (TLS close_notify when applicable).
        void close(const std::error_code &reason = {}) {
            auto self = this->shared_from_this();
            asio::post(stream_.get_executor(), [self, reason] {
                if (self->closed_) return;
                self->close_after_flush_ = true;
                self->close_reason_ = reason;
                if (!self->writing_) self->do_close(reason, true);
            });
        }

        /// Immediate: drops queued data and closes the socket.
        void abort(const std::error_code &reason = {}) {
            auto self = this->shared_from_this();
            asio::post(stream_.get_executor(), [self, reason] { self->do_close(reason, false); });
        }

        /// Closes without notifying anybody (used for attempts that never became active).
        void discard() {
            auto self = this->shared_from_this();
            asio::post(stream_.get_executor(), [self] {
                self->on_close_ = nullptr;
                self->on_data_ = nullptr;
                self->do_close({}, false);
            });
        }

    private:
        struct pending {
            std::vector<uint8_t> vec;
            std::string str;
            bool is_str = false;
            write_callback cb;

            const void *data() const noexcept { return is_str ? static_cast<const void *>(str.data()) : vec.data(); }

            std::size_t size() const noexcept { return is_str ? str.size() : vec.size(); }
        };

        Stream stream_;
        connection_options opts_;
        asio::steady_timer timer_;
        std::vector<uint8_t> rbuf_;
        std::deque<pending> wq_; // deque: push_back/pop_front never move the other elements (batch_ points into them)
        std::vector<asio::const_buffer> batch_;
        std::size_t inflight_ = 0;
        bool writing_ = false;
        bool started_ = false;
        bool closed_ = false;
        bool lingering_ = false; // closed: half-closed socket waiting for the peer's FIN
        bool idle_armed_ = false;
        bool close_after_flush_ = false;
        std::error_code close_reason_;
        steady_clock::time_point last_activity_{};
        std::atomic<bool> open_{false};
        std::atomic<std::size_t> queued_bytes_{0};
        mutable std::mutex ep_mutex_;
        tcp::endpoint local_, remote_;
        data_callback on_data_;
        close_callback on_close_;

        static constexpr std::size_t max_batch_msgs = 16;
        static constexpr std::size_t max_batch_bytes = 256 * 1024;

        auto &socket_layer() { return stream_.lowest_layer(); }

        bool push(pending &&p) {
            const std::size_t n = p.size();
            if (n == 0 || !is_open()) return false;
            if (queued_bytes_.fetch_add(n, std::memory_order_acq_rel) + n > opts_.max_write_queue_bytes) {
                queued_bytes_.fetch_sub(n, std::memory_order_acq_rel);
                return false;
            }
            auto self = this->shared_from_this();
            asio::post(stream_.get_executor(), [self, p = std::move(p)]() mutable { self->enqueue(std::move(p)); });
            return true;
        }

        void do_start() {
            if (closed_ || started_) return;
            started_ = true;
            std::error_code ec;
            auto &sock = socket_layer();
            if (opts_.tcp_no_delay) sock.set_option(tcp::no_delay(true), ec);
            if (opts_.keep_alive) sock.set_option(asio::socket_base::keep_alive(true), ec);
            {
                std::lock_guard l(ep_mutex_);
                local_ = sock.local_endpoint(ec);
                remote_ = sock.remote_endpoint(ec);
            }
            open_.store(true, std::memory_order_release);
            touch();
            arm_idle();
            do_read();
            if (!wq_.empty()) do_write();
        }

        void touch() {
            if (opts_.idle_timeout.count() > 0) last_activity_ = steady_clock::now();
        }

        /// One timer wait per idle period: on expiry it re-arms for the remaining time if there was activity
        /// (instead of cancelling and re-arming the timer on every read and write).
        void arm_idle() {
            if (opts_.idle_timeout.count() <= 0 || closed_ || idle_armed_) return;
            idle_armed_ = true;
            timer_.expires_at(last_activity_ + opts_.idle_timeout);
            timer_.async_wait([self = this->shared_from_this()](const std::error_code &ec) {
                self->idle_armed_ = false;
                if (ec || self->closed_) return;
                if (steady_clock::now() - self->last_activity_ >= self->opts_.idle_timeout)
                    self->do_close(make_error_code(errc::timeout), false);
                else
                    self->arm_idle();
            });
        }

        void do_read() {
            if (closed_ && !lingering_) return;
            stream_.async_read_some(asio::buffer(rbuf_), [self = this->shared_from_this()](const std::error_code &ec, std::size_t n) {
                self->on_read(ec, n);
            });
        }

        void on_read(const std::error_code &ec, std::size_t n) {
            if (lingering_) { // discard until the peer's FIN (or an error / the linger timer)
                if (ec) finish_linger();
                else do_read();
                return;
            }
            if (closed_) return;
            if (ec) {
                bool benign = ec == asio::error::eof;
#ifdef IP_ENABLE_SSL
                if (ec == asio::ssl::error::stream_truncated) benign = true; // peer closed without close_notify
#endif
                if (ec == asio::error::operation_aborted) { do_close(close_reason_, false); return; }
                do_close(benign ? std::error_code{} : ec, false);
                return;
            }
            touch();
            if (on_data_) {
                try { on_data_(rbuf_.data(), n); } catch (...) {}
            }
            if (!closed_ || lingering_) do_read();
        }

        void enqueue(pending &&p) {
            if (closed_) {
                queued_bytes_.fetch_sub(p.size(), std::memory_order_acq_rel);
                if (p.cb) try { p.cb(asio::error::operation_aborted, 0); } catch (...) {}
                return;
            }
            wq_.push_back(std::move(p));
            if (open_.load(std::memory_order_acquire)) do_write();
        }

        void do_write() {
            if (writing_ || wq_.empty() || closed_) return;
            writing_ = true;
            batch_.clear();
            inflight_ = 0;
            std::size_t bytes = 0;
            for (const auto &p: wq_) {
                if (inflight_ == max_batch_msgs || (inflight_ > 0 && bytes + p.size() > max_batch_bytes)) break;
                batch_.emplace_back(p.data(), p.size());
                bytes += p.size();
                ++inflight_;
            }
            asio::async_write(stream_, batch_, [self = this->shared_from_this()](const std::error_code &ec, std::size_t) {
                self->on_written(ec);
            });
        }

        void on_written(const std::error_code &ec) {
            writing_ = false;
            std::vector<pending> done;
            done.reserve(inflight_);
            for (std::size_t i = 0; i < inflight_ && !wq_.empty(); ++i) {
                queued_bytes_.fetch_sub(wq_.front().size(), std::memory_order_acq_rel);
                done.push_back(std::move(wq_.front()));
                wq_.pop_front();
            }
            inflight_ = 0;
            batch_.clear();
            for (auto &p: done) {
                if (!p.cb) continue;
                try { p.cb(ec, ec ? 0 : p.size()); } catch (...) {}
            }
            if (closed_) return;
            if (ec) { do_close(ec, false); return; }
            touch();
            if (!wq_.empty()) do_write();
            else if (close_after_flush_) do_close(close_reason_, true);
        }

        void finish_linger() {
            lingering_ = false;
            std::error_code ig;
            timer_.cancel();
            socket_layer().close(ig);
        }

        void do_close(const std::error_code &reason, bool graceful) {
            if (closed_) return;
            closed_ = true;
            open_.store(false, std::memory_order_release);
            timer_.cancel();
            std::error_code ignored;

            bool done = false;
            if (graceful && started_ && !writing_ && socket_layer().is_open() && opts_.linger_timeout.count() > 0) {
                auto self = this->shared_from_this();
                if constexpr (is_ssl_stream_v<Stream>) {
#ifdef IP_ENABLE_SSL
                    timer_.expires_after(opts_.linger_timeout);
                    timer_.async_wait([self](const std::error_code &e) {
                        if (!e) { std::error_code ig; self->socket_layer().close(ig); }
                    });
                    stream_.async_shutdown([self](const std::error_code &) {
                        std::error_code ig;
                        self->timer_.cancel();
                        self->socket_layer().shutdown(tcp::socket::shutdown_both, ig);
                        self->socket_layer().close(ig);
                    });
                    done = true;
#endif
                } else {
                    // Half-close and keep draining until the peer's FIN: closing a socket that still has unread
                    // data makes the kernel send RST, which can destroy the response that was just flushed.
                    socket_layer().shutdown(tcp::socket::shutdown_send, ignored);
                    if (!ignored) {
                        lingering_ = true; // the read issued by the read loop is still pending and sees this flag
                        timer_.expires_after(opts_.linger_timeout);
                        timer_.async_wait([self](const std::error_code &e) {
                            if (!e && self->lingering_) self->finish_linger();
                        });
                        done = true;
                    }
                }
            }
            if (!done) socket_layer().close(ignored);

            // Fail queued writes, then notify exactly once. Buffers of an in-flight async_write must stay
            // alive until its completion handler runs (the OS may still be reading them): keep them queued.
            std::deque<pending> rest;
            if (writing_) {
                const std::size_t keep = std::min(inflight_, wq_.size());
                for (std::size_t i = keep; i < wq_.size(); ++i) rest.push_back(std::move(wq_[i]));
                wq_.erase(wq_.begin() + static_cast<std::ptrdiff_t>(keep), wq_.end());
            } else {
                rest.swap(wq_);
            }
            for (auto &p: rest) {
                queued_bytes_.fetch_sub(p.size(), std::memory_order_acq_rel);
                if (p.cb) try { p.cb(asio::error::operation_aborted, 0); } catch (...) {}
            }
            auto on_close = std::move(on_close_);
            on_close_ = nullptr;
            on_data_ = nullptr;
            if (on_close) {
                try { on_close(reason); } catch (...) {}
            }
        }
    };
}
