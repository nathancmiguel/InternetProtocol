/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/transport/stream_client.hpp"
#include "ip/utils/httpparser.hpp"
#include "ip/utils/tls.hpp"

namespace internetprotocol {
    namespace detail {
        /// Shared state of an HTTP client: ordered request queue over ONE keep-alive connection.
        template<typename Stream>
        class http_client_state : public std::enable_shared_from_this<http_client_state<Stream>> {
        public:
            using response_cb = std::function<void(const std::error_code &, const http_response_t &)>;

            std::shared_ptr<stream_client<Stream>> link = std::make_shared<stream_client<Stream>>();

            explicit http_client_state(http_limits_t limits = {}) : parser_(http_parser::kind::response, limits) {}

            void init() {
                auto w = this->weak_from_this();
                link->data_hook = [w](const uint8_t *p, std::size_t n) { if (auto s = w.lock()) s->on_data(p, n); };
                link->on_connected = [w] { if (auto s = w.lock()) s->send_front(); };
                link->on_close = [w] { if (auto s = w.lock()) s->on_closed(); };
                link->on_error = [w](const std::error_code &ec) { if (auto s = w.lock()) s->on_error(ec); };
            }

            void set_host(const client_bind_options_t &b) {
                std::lock_guard l(m_);
                bind_ = b;
            }

            std::atomic<uint16_t> request_timeout_seconds{30};

            void request(const http_request_t &req, response_cb cb) {
                {
                    std::lock_guard l(m_);
                    q_.push_back(item{req, std::move(cb), 0});
                }
                pump();
            }

            /// Graceful close; queued requests fail with operation_aborted.
            void close() {
                std::deque<item> dropped;
                {
                    std::lock_guard l(m_);
                    dropped.swap(q_);
                    inflight_ = sent_ = false;
                    ++seq_;
                }
                link->close();
                for (auto &i: dropped) finish(i.cb, asio::error::operation_aborted, {});
            }

            void shutdown() {
                {
                    std::lock_guard l(m_);
                    q_.clear();
                    inflight_ = sent_ = false;
                    ++seq_;
                }
                link->shutdown();
            }

        private:
            struct item {
                http_request_t req;
                response_cb cb;
                int attempts = 0;
            };

            mutable std::mutex m_;
            std::deque<item> q_;
            client_bind_options_t bind_;
            http_parser parser_;
            bool inflight_ = false;  // front item is being processed
            bool sent_ = false;      // front item was written to the wire
            bool got_bytes_ = false;
            bool reused_ = false;
            std::uint64_t seq_ = 0;
            std::error_code last_ec_;
            std::shared_ptr<asio::steady_timer> timer_;

            static void finish(const response_cb &cb, const std::error_code &ec, const http_response_t &res) {
                if (!cb) return;
                try { cb(ec, res); } catch (...) {}
            }

            static bool idempotent(request_method_e m) { return m != POST && m != PATCH; }

            void pump() {
                bool send = false;
                {
                    std::lock_guard l(m_);
                    if (q_.empty() || inflight_) return;
                    inflight_ = true;
                    sent_ = false;
                    reused_ = link->is_open();
                    send = reused_;
                }
                if (send) send_front();
                else connect_link();
            }

            void connect_link() {
                client_bind_options_t b;
                {
                    std::lock_guard l(m_);
                    b = bind_;
                }
                // If the link is mid-close (or already connecting) on_closed()/on_connected() continue the flow.
                link->connect(b);
            }

            void send_front() {
                http_request_t req;
                client_bind_options_t b;
                std::uint64_t id;
                {
                    std::lock_guard l(m_);
                    if (q_.empty() || !inflight_ || sent_) return;
                    req = q_.front().req;
                    b = bind_;
                    sent_ = true;
                    got_bytes_ = false;
                    last_ec_.clear();
                    parser_.reset();
                    parser_.expect_no_body(req.method == HEAD);
                    id = ++seq_;
                }
                uint16_t port = 0;
                try { port = static_cast<uint16_t>(std::stoul(b.port)); } catch (...) {}
                const std::string payload = prepare_request(req, b.address, port);
                arm_timer(id);
                if (!link->write(reinterpret_cast<const uint8_t *>(payload.data()), payload.size()))
                    fail_front(make_error_code(errc::not_connected), id);
            }

            void arm_timer(std::uint64_t id) {
                const auto secs = request_timeout_seconds.load();
                if (secs == 0) return;
                auto t = std::make_shared<asio::steady_timer>(runtime::instance().context());
                {
                    std::lock_guard l(m_);
                    if (timer_) timer_->cancel();
                    timer_ = t;
                }
                t->expires_after(std::chrono::seconds(secs));
                t->async_wait([w = this->weak_from_this(), id, t](const std::error_code &ec) {
                    if (ec) return;
                    if (auto s = w.lock()) {
                        if (s->fail_front(make_error_code(errc::timeout), id)) s->link->abort();
                    }
                });
            }

            void cancel_timer_locked() {
                if (timer_) timer_->cancel();
                timer_.reset();
            }

            /// Completes the front request with an error. `id` guards against stale timers (0 = any).
            bool fail_front(const std::error_code &ec, std::uint64_t id = 0) {
                item it;
                {
                    std::lock_guard l(m_);
                    if (q_.empty() || !inflight_ || (id && id != seq_)) return false;
                    it = std::move(q_.front());
                    q_.pop_front();
                    inflight_ = sent_ = false;
                    ++seq_;
                    cancel_timer_locked();
                }
                finish(it.cb, ec, {});
                pump();
                return true;
            }

            void on_data(const uint8_t *p, std::size_t n) {
                std::size_t off = 0;
                while (off < n) {
                    item done_item;
                    http_response_t res;
                    std::error_code ec;
                    bool completed = false, close_link = false;
                    {
                        std::lock_guard l(m_);
                        if (!inflight_ || !sent_) return; // unsolicited bytes are ignored
                        got_bytes_ = true;
                        off += parser_.feed(p + off, n - off, ec);
                        if (!ec && parser_.done()) {
                            const int sc = parser_.response().status_code;
                            if (sc >= 100 && sc < 200 && sc != 101) { // interim response: keep waiting
                                parser_.reset();
                                parser_.expect_no_body(q_.front().req.method == HEAD);
                                continue;
                            }
                            res = std::move(parser_.response());
                            close_link = header_has_token(res.headers, "connection", "close") ||
                                         (res.version == "1.0" && !header_has_token(res.headers, "connection", "keep-alive"));
                            completed = true;
                        } else if (ec) {
                            completed = true;
                        }
                        if (completed) {
                            done_item = std::move(q_.front());
                            q_.pop_front();
                            inflight_ = sent_ = false;
                            ++seq_;
                            cancel_timer_locked();
                        }
                    }
                    if (!completed) return;
                    if (ec || close_link) link->close();
                    finish(done_item.cb, ec, ec ? http_response_t{} : res);
                    pump();
                    return; // one response per request; trailing bytes are discarded
                }
            }

            void on_error(const std::error_code &ec) {
                bool never_sent;
                {
                    std::lock_guard l(m_);
                    never_sent = inflight_ && !sent_;
                    if (!never_sent) last_ec_ = ec;
                }
                if (never_sent) fail_front(ec);
            }

            void on_closed() {
                item it;
                http_response_t res;
                std::error_code ec;
                bool complete = false, retry = false, reconnect = false;
                {
                    std::lock_guard l(m_);
                    if (!inflight_) return;
                    if (!sent_) { reconnect = true; }
                    else {
                        ec = last_ec_ ? last_ec_ : make_error_code(errc::closed_by_peer);
                        std::error_code eof_ec;
                        if (parser_.reads_until_close()) {
                            parser_.on_eof(eof_ec);
                            res = std::move(parser_.response());
                            complete = true;
                            ec.clear();
                        } else if (!got_bytes_ && reused_ && q_.front().attempts == 0 && idempotent(q_.front().req.method)) {
                            ++q_.front().attempts;
                            inflight_ = sent_ = false;
                            ++seq_;
                            cancel_timer_locked();
                            retry = true;
                        }
                        if (complete || !retry) {
                            it = std::move(q_.front());
                            q_.pop_front();
                            inflight_ = sent_ = false;
                            ++seq_;
                            cancel_timer_locked();
                        }
                    }
                }
                if (reconnect) { connect_link(); return; }
                if (!retry) finish(it.cb, ec, res);
                pump();
            }
        };
    }

    /**
     * HTTP/1.1 client (plain or TLS) over one persistent connection. Requests are queued and
     * executed strictly in order; the connection is (re)opened on demand, idempotent requests that
     * hit a stale keep-alive connection are retried once, responses may use Content-Length, chunked
     * encoding or read-until-close.
     *
     * @code
     * http_client_c client;
     * client.set_host({"example.com", "80", v4});
     * http_request_t req; req.path = "/";
     * client.request(req, [](const asio::error_code &ec, const http_response_t &res) { ... });
     * @endcode
     */
    template<typename Stream>
    class basic_http_client {
    public:
        using response_callback = std::function<void(const asio::error_code &, const http_response_t &)>;

        basic_http_client() : basic_http_client(std::make_shared<detail::http_client_state<Stream>>()) {}

        basic_http_client(const basic_http_client &) = delete;

        basic_http_client &operator=(const basic_http_client &) = delete;

        ~basic_http_client() { state_->shutdown(); }

        /// Close the keep-alive connection after this many idle seconds (0 = never). Applied on the next (re)connect.
        uint16_t idle_timeout_seconds = 0;

        /// Max seconds to wait for a complete response (0 = no limit). Default 30.
        void set_request_timeout(uint16_t seconds) { state_->request_timeout_seconds.store(seconds); }

        bool is_open() const { return state_->link->is_open(); }

        client_options_t &options() { return state_->link->options(); }

        void set_host(const client_bind_options_t &bind_opts = {}) { state_->set_host(bind_opts); }

        /// Queues a request. The callback receives the parsed response or an error (timeout, connection closed, ...).
        void request(const http_request_t &req, const response_callback &response_cb) {
            if (!state_->link->is_open())
                state_->link->options().idle_timeout = std::chrono::seconds(idle_timeout_seconds);
            state_->request(req, response_cb);
        }

        /// Closes the connection; pending requests complete with operation_aborted.
        void close() { state_->close(); }

    protected:
        explicit basic_http_client(std::shared_ptr<detail::http_client_state<Stream>> st) : state_(std::move(st)) {
            state_->init();
        }

        std::shared_ptr<detail::http_client_state<Stream>> state_;
    };

    using http_client_c = basic_http_client<plain_stream>;

#ifdef IP_ENABLE_SSL
    class http_client_ssl_c : public basic_http_client<ssl_stream> {
    public:
        explicit http_client_ssl_c(const security_context_opts &sec_opts = {})
            : basic_http_client<ssl_stream>(make_state(sec_opts)) {}

    private:
        static std::shared_ptr<detail::http_client_state<ssl_stream>> make_state(const security_context_opts &o) {
            auto st = std::make_shared<detail::http_client_state<ssl_stream>>();
            std::error_code ec;
            auto ctx = make_client_ssl_context(o, ec);
            st->link->set_tls(std::move(ctx), o, ec);
            return st;
        }
    };
#endif
}
