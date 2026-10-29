/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"

namespace internetprotocol {
    /**
     * Process wide I/O runtime: ONE io_context served by a fixed pool of worker threads.
     *
     * Every client/server/connection lives on this context and serialises its own handlers
     * with a strand. (The previous design gave each object its own io_context and blocked one
     * pool thread per object, starving every object beyond hardware_concurrency().)
     */
    class runtime {
    public:
        static runtime &instance() {
            static runtime r;
            return r;
        }

        runtime(const runtime &) = delete;

        runtime &operator=(const runtime &) = delete;

        asio::io_context &context() noexcept { return ctx_; }

        /// Worker count used on the next start (0 = automatic). Ignored while running.
        void set_thread_count(std::size_t n) {
            std::lock_guard lock(mutex_);
            requested_threads_ = n;
        }

        /// Starts the worker threads if needed. Called lazily by every connect()/open().
        void start() {
            std::lock_guard lock(mutex_);
            if (running_) return;
            if (ctx_.stopped()) ctx_.restart();
            guard_.emplace(asio::make_work_guard(ctx_));
            std::size_t n = requested_threads_;
            if (n == 0) n = std::max<std::size_t>(2, std::thread::hardware_concurrency());
            threads_.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                threads_.emplace_back([this] { worker(); });
            running_ = true;
        }

        bool is_running() const {
            std::lock_guard lock(mutex_);
            return running_;
        }

        /// Blocks until all outstanding asynchronous operations finished, then joins the workers.
        void join() {
            auto threads = release(false);
            join_all(threads);
        }

        /// Stops the context as soon as possible; pending handlers are discarded.
        void stop() {
            auto threads = release(true);
            join_all(threads);
        }

        ~runtime() { stop(); }

    private:
        runtime() = default;

        std::vector<std::thread> release(bool hard) {
            std::vector<std::thread> threads;
            std::lock_guard lock(mutex_);
            if (!running_) return threads;
            guard_.reset();
            if (hard) ctx_.stop();
            threads.swap(threads_);
            running_ = false;
            return threads;
        }

        static void join_all(std::vector<std::thread> &threads) {
            for (auto &t: threads) {
                if (!t.joinable()) continue;
                if (t.get_id() == std::this_thread::get_id()) t.detach(); // called from a handler
                else t.join();
            }
        }

        void worker() {
            for (;;) {
                try {
                    ctx_.run();
                    break;
                } catch (...) {
                    // keep the worker alive if a completion handler throws
                }
            }
        }

        asio::io_context ctx_;
        mutable std::mutex mutex_;
        std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
        std::vector<std::thread> threads_;
        std::size_t requested_threads_ = 0;
        bool running_ = false;
    };

    using strand_t = asio::strand<asio::io_context::executor_type>;

    inline strand_t make_runtime_strand() {
        runtime::instance().start();
        return asio::make_strand(runtime::instance().context());
    }

    /// Stops the threads as soon as possible. Pending function objects may never be invoked.
    inline void stop_threads() { runtime::instance().stop(); }

    /// Blocks until all outstanding async work completed and the workers exited.
    /// The runtime restarts automatically on the next connect()/open().
    inline void join_threads() { runtime::instance().join(); }
}
