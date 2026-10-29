/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

// ASIO is always used in standalone (non-Boost) mode.
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE 1
#endif

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

// Both spellings are accepted: ENABLE_SSL (legacy) and IP_ENABLE_SSL (CMake).
#if defined(ENABLE_SSL) && !defined(IP_ENABLE_SSL)
#define IP_ENABLE_SSL 1
#endif
#if defined(IP_ENABLE_SSL) && !defined(ENABLE_SSL)
#define ENABLE_SSL 1
#endif

#include <asio.hpp>
#ifdef IP_ENABLE_SSL
#include <asio/ssl.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define IP_VERSION_MAJOR 2
#define IP_VERSION_MINOR 0
#define IP_VERSION_PATCH 0

namespace internetprotocol {
    using asio::ip::tcp;
    using asio::ip::udp;
    using asio::ip::make_address;
    using asio::ip::address;
    using asio::ip::address_v4;
    using asio::ip::address_v6;

    using error_code = std::error_code;
    using steady_clock = std::chrono::steady_clock;
    using milliseconds = std::chrono::milliseconds;
}
