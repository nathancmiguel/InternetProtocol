/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"

namespace internetprotocol {
    template<typename Signature>
    class event;

    /**
     * Thread-safe single-subscriber event.
     *
     * Keeps the `object.on_xxx = [](...) {...};` syntax but, unlike a plain std::function
     * member, is safe to (re)assign while network threads run: the handler is an immutable
     * shared_ptr; invocation copies it under a short lock and calls it OUTSIDE the lock
     * (no deadlock if the handler re-assigns the event or closes the connection).
     * Exceptions thrown by user handlers are contained.
     */
    template<typename... Args>
    class event<void(Args...)> {
    public:
        using function_type = std::function<void(Args...)>;

        event() = default;

        event(const event &) = delete;

        event &operator=(const event &) = delete;

        template<typename F>
            requires (!std::is_same_v<std::remove_cvref_t<F>, event> &&
                      !std::is_null_pointer_v<std::remove_cvref_t<F>> &&
                      std::is_constructible_v<function_type, F>)
        event &operator=(F &&f) {
            function_type fn(std::forward<F>(f));
            std::shared_ptr<const function_type> p;
            if (fn) p = std::make_shared<const function_type>(std::move(fn));
            std::lock_guard lock(mutex_);
            handler_.swap(p);
            return *this;
        }

        event &operator=(std::nullptr_t) {
            reset();
            return *this;
        }

        void reset() {
            std::shared_ptr<const function_type> old;
            std::lock_guard lock(mutex_);
            handler_.swap(old);
        }

        explicit operator bool() const {
            std::lock_guard lock(mutex_);
            return handler_ != nullptr;
        }

        template<typename... U>
        void operator()(U &&... args) const noexcept {
            std::shared_ptr<const function_type> h;
            {
                std::lock_guard lock(mutex_);
                h = handler_;
            }
            if (!h) return;
            try {
                (*h)(std::forward<U>(args)...);
            } catch (...) {
                // A throwing user handler must never unwind through ASIO worker threads.
            }
        }

    private:
        mutable std::mutex mutex_;
        std::shared_ptr<const function_type> handler_;
    };
}
