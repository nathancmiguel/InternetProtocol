/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/runtime.hpp"
#include "ip/utils/dataframe.hpp"
#include "ip/utils/handshake.hpp"
#include "ip/utils/string.hpp"

namespace internetprotocol {
    /// Tunables shared by WebSocket clients/servers/remotes (thread-safe, may change at any time).
    struct ws_settings {
        std::atomic<std::size_t> max_message_bytes{16u * 1024u * 1024u};
        std::atomic<uint16_t> ping_interval_seconds{0}; ///< 0 = no automatic keep-alive ping
        std::atomic<uint32_t> close_timeout_ms{3000};   ///< wait for the peer's CLOSE frame in end()/close()
    };

    namespace detail {
        /**
         * RFC 6455 protocol engine (post-handshake): frame decoding, fragmentation reassembly,
         * UTF-8 validation, ping/pong, close handshake and heartbeat. Transport agnostic: the
         * owner provides send/close hooks. All inbound processing runs on the connection strand;
         * the public send/close methods are thread-safe.
         */
        class ws_engine : public std::enable_shared_from_this<ws_engine> {
        public:
            using write_cb = std::function<void(const std::error_code &, std::size_t)>;

            struct hooks {
                std::function<bool(std::vector<uint8_t> &&, write_cb)> send;
                std::function<void()> close_transport; ///< graceful (flush then close)
                std::function<void()> abort_transport;
                std::function<void(std::vector<uint8_t> &&, bool)> on_message;
                std::function<void()> on_ping;
                std::function<void()> on_pong;
            };

            ws_engine(asio::any_io_executor ex, bool is_client, hooks h, std::shared_ptr<ws_settings> settings)
                : ex_(std::move(ex)), client_(is_client), h_(std::move(h)), settings_(std::move(settings)),
                  parser_(settings_->max_message_bytes.load(), !is_client), timer_(ex_), hb_timer_(ex_) {}

            close_state_e state() const noexcept { return state_.load(std::memory_order_acquire); }

            bool is_open() const noexcept { return state() == OPEN; }

            /// Sends one frame carrying a text/binary message (or a continuation when df.opcode is CONTINUATION_FRAME).
            bool send_message(const uint8_t *data, std::size_t n, bool binary, const dataframe_t &df, write_cb cb) {
                if (!is_open()) return false;
                dataframe_t f = df;
                f.mask = client_; // clients MUST mask, servers MUST NOT (RFC 6455 §5.1)
                f.rsv1 = f.rsv2 = f.rsv3 = false;
                if (f.opcode != CONTINUATION_FRAME) f.opcode = binary ? BINARY_FRAME : TEXT_FRAME;
                return h_.send(encode_frame(data, n, f), std::move(cb));
            }

            bool send_control(opcode_e op, const std::vector<uint8_t> &payload, write_cb cb) {
                if (!is_open() || payload.size() > 125) return false;
                dataframe_t f;
                f.mask = client_;
                f.opcode = op;
                std::vector<uint8_t> frame = encode_frame(payload.data(), payload.size(), f);
                return h_.send(std::move(frame), std::move(cb));
            }

            /// Starts the closing handshake. `wait` => wait (bounded) for the peer's CLOSE before dropping TCP.
            void close(uint16_t code, std::string reason, bool wait) {
                asio::post(ex_, [self = shared_from_this(), code, reason = std::move(reason), wait] {
                    if (self->state() != OPEN) return;
                    self->local_code_ = code;
                    self->send_close(code, reason);
                    if (wait) {
                        self->timer_.expires_after(std::chrono::milliseconds(self->settings_->close_timeout_ms.load()));
                        self->timer_.async_wait([self](const std::error_code &ec) {
                            if (!ec && self->state() != CLOSED) self->h_.abort_transport();
                        });
                    } else {
                        self->h_.close_transport();
                    }
                });
            }

            void start_heartbeat() {
                const auto secs = settings_->ping_interval_seconds.load();
                if (secs == 0) return;
                asio::post(ex_, [self = shared_from_this(), secs] { self->heartbeat(secs); });
            }

            /// Feed raw bytes (strand only).
            void on_data(const uint8_t *p, std::size_t n) {
                if (failed_ || peer_closed_) return;
                const uint16_t err = parser_.feed(p, n, [this](ws_frame &&f) {
                    if (!failed_ && !peer_closed_) handle_frame(std::move(f));
                });
                if (err && !failed_) fail(err, err == 1009 ? "message too big" : "protocol error");
            }

            /// Called by the owner when the transport is gone. Returns the final close status.
            std::pair<uint16_t, std::string> on_transport_closed() {
                state_.store(CLOSED, std::memory_order_release);
                timer_.cancel();
                hb_timer_.cancel();
                if (peer_closed_) return {peer_code_, peer_reason_};
                if (failed_ || local_code_) return {local_code_ ? local_code_ : uint16_t(1006), local_reason_};
                return {1006, ""};
            }

        private:
            asio::any_io_executor ex_;
            bool client_;
            hooks h_;
            std::shared_ptr<ws_settings> settings_;
            ws_frame_parser parser_;
            asio::steady_timer timer_, hb_timer_;
            std::atomic<close_state_e> state_{OPEN};
            std::vector<uint8_t> msg_;
            uint8_t frag_op_ = 0;
            bool frag_active_ = false;
            bool failed_ = false, peer_closed_ = false, close_sent_ = false, awaiting_pong_ = false;
            uint16_t peer_code_ = 1005, local_code_ = 0;
            std::string peer_reason_, local_reason_;

            void send_close(uint16_t code, const std::string &reason) {
                if (close_sent_) return;
                close_sent_ = true;
                local_reason_ = reason;
                state_.store(CLOSING, std::memory_order_release);
                dataframe_t f;
                f.mask = client_;
                f.opcode = CLOSE_FRAME;
                const auto payload = make_close_payload(code, reason);
                h_.send(encode_frame(payload.data(), payload.size(), f), nullptr);
            }

            void fail(uint16_t code, const char *reason) {
                failed_ = true;
                local_code_ = code;
                send_close(code, reason);
                h_.close_transport();
            }

            void heartbeat(uint16_t secs) {
                if (state() != OPEN) return;
                if (awaiting_pong_) { h_.abort_transport(); return; } // peer silent for a full interval
                awaiting_pong_ = true;
                send_control(PING, {}, nullptr);
                hb_timer_.expires_after(std::chrono::seconds(secs));
                hb_timer_.async_wait([self = shared_from_this(), secs](const std::error_code &ec) {
                    if (!ec) self->heartbeat(secs);
                });
            }

            void handle_frame(ws_frame &&f) {
                awaiting_pong_ = false; // any traffic proves liveness
                switch (f.opcode) {
                    case CLOSE_FRAME: handle_close(f); return;
                    case PING:
                        if (state() == OPEN) send_control(PONG, f.payload, nullptr);
                        if (h_.on_ping) h_.on_ping();
                        return;
                    case PONG:
                        if (h_.on_pong) h_.on_pong();
                        return;
                    case TEXT_FRAME:
                    case BINARY_FRAME:
                        if (frag_active_) { fail(1002, "new data frame inside fragmented message"); return; }
                        frag_op_ = f.opcode;
                        msg_ = std::move(f.payload);
                        frag_active_ = !f.fin;
                        if (f.fin) deliver();
                        return;
                    case CONTINUATION_FRAME:
                        if (!frag_active_) { fail(1002, "unexpected continuation frame"); return; }
                        if (msg_.size() + f.payload.size() > settings_->max_message_bytes.load()) { fail(1009, "message too big"); return; }
                        msg_.insert(msg_.end(), f.payload.begin(), f.payload.end());
                        if (f.fin) deliver();
                        return;
                    default:
                        fail(1002, "unknown opcode");
                }
            }

            void deliver() {
                frag_active_ = false;
                const bool binary = frag_op_ == BINARY_FRAME;
                if (!binary && !is_valid_utf8(msg_.data(), msg_.size())) { fail(1007, "invalid UTF-8"); return; }
                auto m = std::move(msg_);
                msg_.clear();
                if (state() == OPEN && h_.on_message) h_.on_message(std::move(m), binary);
            }

            void handle_close(const ws_frame &f) {
                uint16_t code = 1005;
                std::string reason;
                if (f.payload.size() == 1) { fail(1002, "invalid close payload"); return; }
                if (f.payload.size() >= 2) {
                    code = static_cast<uint16_t>((f.payload[0] << 8) | f.payload[1]);
                    if (!is_valid_close_code(code)) { fail(1002, "invalid close code"); return; }
                    reason.assign(f.payload.begin() + 2, f.payload.end());
                    if (!is_valid_utf8(f.payload.data() + 2, f.payload.size() - 2)) { fail(1007, "invalid UTF-8 in close reason"); return; }
                }
                peer_closed_ = true;
                peer_code_ = code;
                peer_reason_ = std::move(reason);
                timer_.cancel();
                if (!close_sent_) send_close(code == 1005 ? uint16_t(1000) : code, ""); // echo
                h_.close_transport();
            }
        };
    }
}
