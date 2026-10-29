/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"

namespace internetprotocol {
    /**
     * Splits `str` into views of at most `size` bytes. Zero copy: the returned buffers point INTO `str`,
     * which must outlive them. (The old version returned views of a local temporary: dangling.)
     */
    inline std::vector<asio::const_buffer> slice_string(const std::string &str, const size_t size) {
        std::vector<asio::const_buffer> buffers;
        if (size == 0 || str.empty()) return buffers;
        buffers.reserve(str.size() / size + 1);
        for (size_t off = 0; off < str.size(); off += size)
            buffers.emplace_back(str.data() + off, std::min(size, str.size() - off));
        return buffers;
    }

    /// Same as slice_string for byte vectors. `buf` must outlive the returned buffers.
    inline std::vector<asio::const_buffer> slice_buffer(const std::vector<uint8_t> &buf, const size_t size) {
        std::vector<asio::const_buffer> buffers;
        if (size == 0 || buf.empty()) return buffers;
        buffers.reserve(buf.size() / size + 1);
        for (size_t off = 0; off < buf.size(); off += size)
            buffers.emplace_back(buf.data() + off, std::min(size, buf.size() - off));
        return buffers;
    }
}
