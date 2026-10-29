/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include <random>

#include "ip/core/types.hpp"

namespace internetprotocol {
    /// Fresh 4-byte masking key per frame. The engine is seeded once per thread (the old code
    /// constructed std::random_device + mt19937 for EVERY frame).
    inline std::array<uint8_t, 4> mask_gen() {
        thread_local std::mt19937_64 gen{[] {
            std::random_device rd;
            std::seed_seq seq{rd(), rd(), rd(), rd()};
            return std::mt19937_64(seq);
        }()};
        const uint64_t r = gen();
        return {uint8_t(r), uint8_t(r >> 8), uint8_t(r >> 16), uint8_t(r >> 24)};
    }

    /// Builds one complete frame (header + optionally masked payload) in a single allocation.
    inline std::vector<uint8_t> encode_frame(const uint8_t *payload, size_t len, const dataframe_t &df) {
        const size_t header = 2 + (len > 65535 ? 8 : len > 125 ? 2 : 0) + (df.mask ? 4 : 0);
        std::vector<uint8_t> out;
        out.reserve(header + len);
        uint8_t b1 = df.fin ? 0x80 : 0x00;
        if (df.rsv1) b1 |= RSV1;
        if (df.rsv2) b1 |= RSV2;
        if (df.rsv3) b1 |= RSV3;
        b1 |= static_cast<uint8_t>(df.opcode) & 0x0F;
        out.push_back(b1);
        const uint8_t mbit = df.mask ? 0x80 : 0x00;
        if (len <= 125) {
            out.push_back(static_cast<uint8_t>(mbit | len));
        } else if (len <= 65535) {
            out.push_back(mbit | 126);
            out.push_back(static_cast<uint8_t>(len >> 8));
            out.push_back(static_cast<uint8_t>(len));
        } else {
            out.push_back(mbit | 127);
            for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(len) >> (8 * i)));
        }
        if (df.mask) {
            const auto key = mask_gen();
            out.insert(out.end(), key.begin(), key.end());
            const size_t base = out.size();
            out.resize(base + len);
            for (size_t i = 0; i < len; ++i) out[base + i] = payload[i] ^ key[i & 3];
        } else if (len) {
            out.insert(out.end(), payload, payload + len);
        }
        return out;
    }

    inline std::string encode_string_payload(const std::string &payload, const dataframe_t &dataframe) {
        auto v = encode_frame(reinterpret_cast<const uint8_t *>(payload.data()), payload.size(), dataframe);
        return std::string(v.begin(), v.end());
    }

    inline std::vector<uint8_t> encode_buffer_payload(const std::vector<uint8_t> &payload, const dataframe_t &dataframe) {
        return encode_frame(payload.data(), payload.size(), dataframe);
    }

    /// Builds a CLOSE frame payload: 2 byte status code + UTF-8 reason (truncated to 123 bytes).
    inline std::vector<uint8_t> make_close_payload(uint16_t code, std::string_view reason) {
        std::vector<uint8_t> p;
        if (code == 0 || code == 1005 || code == 1006) return p; // reserved: must not be sent
        reason = reason.substr(0, 123);
        p.reserve(2 + reason.size());
        p.push_back(static_cast<uint8_t>(code >> 8));
        p.push_back(static_cast<uint8_t>(code));
        p.insert(p.end(), reason.begin(), reason.end());
        return p;
    }

    /// Is this close code legal on the wire (RFC 6455 §7.4)?
    inline bool is_valid_close_code(uint16_t c) {
        return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
    }

    /**
     * Decodes ONE complete frame from the start of `buffer`. Returns false if the frame is
     * incomplete or invalid. Bounds-checked (no giant reserve from a hostile length field).
     */
    inline bool decode_payload(const std::vector<uint8_t> &buffer, std::vector<uint8_t> &payload,
                               dataframe_t &dataframe) {
        size_t pos = 0;
        if (buffer.size() < 2) return false;
        const uint8_t byte1 = buffer[pos++];
        dataframe.fin = byte1 & 0x80;
        dataframe.rsv1 = byte1 & 0x40;
        dataframe.rsv2 = byte1 & 0x20;
        dataframe.rsv3 = byte1 & 0x10;
        dataframe.opcode = static_cast<opcode_e>(byte1 & 0x0F);
        const uint8_t byte2 = buffer[pos++];
        dataframe.mask = byte2 & 0x80;
        uint64_t len = byte2 & 0x7F;
        if (len == 126) {
            if (buffer.size() < pos + 2) return false;
            len = (static_cast<uint64_t>(buffer[pos]) << 8) | buffer[pos + 1];
            pos += 2;
        } else if (len == 127) {
            if (buffer.size() < pos + 8) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | buffer[pos + i];
            pos += 8;
            if (len >> 63) return false;
        }
        dataframe.length = static_cast<size_t>(len);
        if (dataframe.mask) {
            if (buffer.size() < pos + 4) return false;
            for (int i = 0; i < 4; ++i) dataframe.masking_key[i] = buffer[pos++];
        }
        if (buffer.size() - pos < len) return false;
        payload.assign(buffer.begin() + pos, buffer.begin() + pos + static_cast<size_t>(len));
        if (dataframe.mask)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= dataframe.masking_key[i & 3];
        return true;
    }

    /// One decoded frame (payload already unmasked).
    struct ws_frame {
        bool fin = true;
        bool rsv = false;
        uint8_t opcode = 0;
        bool masked = false;
        std::vector<uint8_t> payload;
    };

    /**
     * Incremental frame decoder. Feed arbitrary byte chunks; completed frames are handed to
     * `on_frame`. Returns 0 on success or the RFC 6455 close code to send on a violation
     * (1002 protocol error, 1009 message too big).
     */
    class ws_frame_parser {
    public:
        explicit ws_frame_parser(size_t max_frame_bytes = 16u * 1024u * 1024u, bool require_masked = false)
            : max_frame_(max_frame_bytes), require_masked_(require_masked) {}

        void reset() { hdr_.clear(); in_payload_ = false; cur_ = {}; got_ = 0; }

        template<typename OnFrame>
        uint16_t feed(const uint8_t *d, size_t n, OnFrame &&on_frame) {
            size_t i = 0;
            while (i < n) {
                if (!in_payload_) {
                    hdr_.push_back(d[i++]);
                    if (!header_ready()) continue;
                    if (const uint16_t err = parse_header()) return err;
                    if (len_ == 0) {
                        on_frame(std::move(cur_));
                        reset();
                    }
                    continue;
                }
                const size_t take = std::min(n - i, static_cast<size_t>(len_ - got_));
                cur_.payload.insert(cur_.payload.end(), d + i, d + i + take);
                got_ += take;
                i += take;
                if (got_ == len_) {
                    if (cur_.masked)
                        for (size_t k = 0; k < cur_.payload.size(); ++k) cur_.payload[k] ^= key_[k & 3];
                    on_frame(std::move(cur_));
                    reset();
                }
            }
            return 0;
        }

    private:
        size_t max_frame_;
        bool require_masked_;
        std::vector<uint8_t> hdr_;
        bool in_payload_ = false;
        ws_frame cur_;
        uint64_t len_ = 0;
        uint64_t got_ = 0;
        std::array<uint8_t, 4> key_{};

        bool header_ready() const {
            if (hdr_.size() < 2) return false;
            const uint8_t l7 = hdr_[1] & 0x7F;
            const size_t need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((hdr_[1] & 0x80) ? 4 : 0);
            return hdr_.size() >= need;
        }

        uint16_t parse_header() {
            const uint8_t b1 = hdr_[0], b2 = hdr_[1];
            cur_.fin = b1 & 0x80;
            cur_.rsv = b1 & 0x70;
            cur_.opcode = b1 & 0x0F;
            cur_.masked = b2 & 0x80;
            size_t pos = 2;
            uint64_t len = b2 & 0x7F;
            if (len == 126) {
                len = (static_cast<uint64_t>(hdr_[2]) << 8) | hdr_[3];
                pos += 2;
            } else if (len == 127) {
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | hdr_[2 + i];
                pos += 8;
                if (len >> 63) return 1002;
            }
            if (cur_.masked) for (int i = 0; i < 4; ++i) key_[i] = hdr_[pos++];
            if (cur_.rsv) return 1002;                                   // no extensions negotiated
            const bool control = cur_.opcode & 0x08;
            if ((cur_.opcode > 0x02 && cur_.opcode < 0x08) || cur_.opcode > 0x0A) return 1002; // reserved opcodes
            if (control && (!cur_.fin || len > 125)) return 1002;
            if (require_masked_ && !cur_.masked) return 1002;
            if (!require_masked_ && cur_.masked) return 1002;
            if (len > max_frame_) return 1009;
            len_ = len;
            got_ = 0;
            in_payload_ = len > 0;
            cur_.payload.clear();
            cur_.payload.reserve(static_cast<size_t>(len));
            return 0;
        }
    };
}
