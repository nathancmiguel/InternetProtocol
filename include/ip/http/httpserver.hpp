/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/http/httpremote.hpp"
#include "ip/transport/stream_server.hpp"
#include "ip/utils/tls.hpp"

namespace internetprotocol {
    namespace detail {
        template<typename Remote>
        class http_router {
        public:
            using handler = std::function<void(const http_request_t &, const std::shared_ptr<Remote> &)>;

            void add(int slot, const std::string &path, handler cb) {
                std::lock_guard l(m_);
                if (cb) routes_[slot][path] = std::make_shared<const handler>(std::move(cb));
                else routes_[slot].erase(path);
            }

            /// Specific method first; falls back to `all`. 404 when the path is unknown, 405 when only other methods exist.
            void dispatch(const http_request_t &req, const std::shared_ptr<Remote> &remote) const {
                std::shared_ptr<const handler> h;
                std::string allow;
                {
                    std::lock_guard l(m_);
                    const int slot = slot_for(req.method);
                    if (slot >= 0) {
                        auto it = routes_[slot].find(req.path);
                        if (it != routes_[slot].end()) h = it->second;
                    }
                    if (!h && req.method == HEAD) { // RFC 9110 §9.3.2: HEAD is answered by the GET handler (body stripped)
                        auto it = routes_[2].find(req.path);
                        if (it != routes_[2].end()) h = it->second;
                    }
                    if (!h) {
                        auto it = routes_[0].find(req.path);
                        if (it != routes_[0].end()) h = it->second;
                    }
                    if (!h) {
                        static constexpr const char *names[] = {"", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT"};
                        for (int s = 1; s < 8; ++s)
                            if (routes_[s].count(req.path)) allow += (allow.empty() ? "" : ", ") + std::string(names[s]);
                    }
                }
                if (h) {
                    try { (*h)(req, remote); } catch (...) {
                        remote->response.status_code = 500;
                        remote->response.status_message.clear();
                        remote->response.body = "Internal Server Error";
                        remote->write();
                    }
                    return;
                }
                if (!allow.empty()) {
                    remote->response.status_code = 405;
                    remote->response.headers["Allow"] = allow;
                    remote->response.body = "Method Not Allowed";
                } else {
                    remote->response.status_code = 404;
                    remote->response.body = "Not Found";
                }
                remote->response.status_message.clear();
                remote->write();
            }

        private:
            static int slot_for(request_method_e m) {
                switch (m) {
                    case DEL: return 1; case GET: return 2; case HEAD: return 3; case OPTIONS: return 4;
                    case PATCH: return 5; case POST: return 6; case PUT: return 7; default: return -1;
                }
            }

            mutable std::mutex m_;
            std::unordered_map<std::string, std::shared_ptr<const handler>> routes_[8];
        };

        template<typename Stream>
        class http_server_core : public stream_server<Stream, basic_http_remote<Stream>> {
        public:
            using remote_t = basic_http_remote<Stream>;
            using base = stream_server<Stream, remote_t>;

            explicit http_server_core(std::shared_ptr<http_router<remote_t>> router, http_limits_t limits)
                : base([limits](std::shared_ptr<connection<Stream>> c) {
                      return std::make_shared<remote_t>(std::move(c), limits);
                  }), router_(std::move(router)) {}

        protected:
            void prepare_remote(const std::shared_ptr<remote_t> &r) override {
                r->on_request = [router = router_, w = std::weak_ptr<remote_t>(r)](const http_request_t &req) {
                    if (auto remote = w.lock()) router->dispatch(req, remote);
                };
            }

        private:
            std::shared_ptr<http_router<remote_t>> router_;
        };
    }

    /**
     * HTTP server (plain or TLS) with exact-path routing.
     *
     * @code
     * http_server_c server;
     * server.get("/", [](const http_request_t &req, const std::shared_ptr<http_remote_c> &res) {
     *     res->response.body = "Hello";
     *     res->write();
     * });
     * server.open({"", 8080, v4, true});
     * @endcode
     */
    template<typename Stream>
    class basic_http_server {
    public:
        using remote_t = basic_http_remote<Stream>;
        using router_t = detail::http_router<remote_t>;
        using core_t = detail::http_server_core<Stream>;
        using route_callback = std::function<void(const http_request_t &, const std::shared_ptr<remote_t> &)>;

    protected:
        // Declared before the public references below: initialisation order matters.
        std::shared_ptr<router_t> router_;
        std::shared_ptr<core_t> core_;

    public:
        basic_http_server() : basic_http_server(make_core()) {}

        basic_http_server(const basic_http_server &) = delete;

        basic_http_server &operator=(const basic_http_server &) = delete;

        ~basic_http_server() { core_->shutdown(); }

        /// Idle timeout (seconds) applied to every connection when open() is called. 0 = disabled.
        uint16_t iddle_timeout = 0;

        bool is_open() const { return core_->is_open(); }

        tcp::endpoint local_endpoint() const { return core_->local_endpoint(); }

        std::vector<std::shared_ptr<remote_t>> clients() const { return core_->clients(); }

        asio::error_code get_error_code() const { return core_->get_error_code(); }

        server_options_t &options() { return core_->options(); }

        void all(const std::string &path, const route_callback &cb) { router_->add(0, path, cb); }

        void del(const std::string &path, const route_callback &cb) { router_->add(1, path, cb); }

        void get(const std::string &path, const route_callback &cb) { router_->add(2, path, cb); }

        void head(const std::string &path, const route_callback &cb) { router_->add(3, path, cb); }

        void options(const std::string &path, const route_callback &cb) { router_->add(4, path, cb); }

        void patch(const std::string &path, const route_callback &cb) { router_->add(5, path, cb); }

        void post(const std::string &path, const route_callback &cb) { router_->add(6, path, cb); }

        void put(const std::string &path, const route_callback &cb) { router_->add(7, path, cb); }

        bool open(const server_bind_options_t &bind_opts = {"", 8080, v4, true}) {
            if (iddle_timeout > 0) core_->options().idle_timeout = std::chrono::seconds(iddle_timeout);
            return core_->open(bind_opts);
        }

        void close() { core_->close(); }

        int &backlog;
        event<void()> &on_listening;
        event<void(const std::shared_ptr<remote_t> &)> &on_client_accepted;
        event<void()> &on_close;
        event<void(const asio::error_code &)> &on_error;

    protected:
        using made = std::pair<std::shared_ptr<core_t>, std::shared_ptr<router_t>>;

        explicit basic_http_server(made m)
            : router_(std::move(m.second)), core_(std::move(m.first)), backlog(core_->backlog), on_listening(core_->on_listening),
              on_client_accepted(core_->on_client_accepted), on_close(core_->on_close), on_error(core_->on_error) {}

        /// Builds core + router pair used by derived (TLS) servers.
        static made make_core(http_limits_t limits = {}) {
            auto router = std::make_shared<router_t>();
            return {std::make_shared<core_t>(router, limits), router};
        }
    };

    using http_server_c = basic_http_server<plain_stream>;

#ifdef IP_ENABLE_SSL
    class http_server_ssl_c : public basic_http_server<ssl_stream> {
    public:
        explicit http_server_ssl_c(const security_context_opts &sec_opts = {})
            : basic_http_server<ssl_stream>(make_core()) {
            std::error_code ec;
            auto ctx = make_server_ssl_context(sec_opts, ec);
            core_->set_tls(std::move(ctx), ec);
        }
    };
#endif
}
