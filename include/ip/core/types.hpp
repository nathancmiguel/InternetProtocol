/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "ip/core/config.hpp"
#include "ip/core/error.hpp"

namespace internetprotocol {
    typedef enum : uint8_t {
        v4 = 0,
        v6 = 1,
        unspecified = 2, ///< accept whichever family the resolver returns
    } protocol_type_e;

    /// Bit flags (may be OR-ed together: verify_peer | verify_fail_if_no_peer_cert).
    typedef enum : uint8_t {
        none = 0x00,
        verify_peer = 0x01,
        verify_fail_if_no_peer_cert = 0x02,
        verify_client_once = 0x04,
    } verify_mode_e;

#ifdef IP_ENABLE_SSL
    typedef asio::ssl::context_base::file_format file_format_e;
#else
    typedef enum { asn1, pem } file_format_e;
#endif

    /**
     * Security context configuration. All material is loaded from memory.
     * NOTE: the first seven fields keep the original order so positional aggregate
     * initialisation `{ key, cert, "", "", file_format_e::pem, none, "" }` still works.
     */
    struct security_context_opts {
        std::string private_key;
        std::string cert;
        std::string cert_chain;
        std::string rsa_private_key;
        file_format_e file_format = file_format_e::pem;
        verify_mode_e verify_mode = verify_peer;
        /// Name used for certificate host verification / SNI. Empty = host passed to connect().
        std::string host_name_verification;
        /// Extra trusted CA certificates (PEM, in memory).
        std::string ca_cert;
        /// Clients also trust the operating system CA store.
        bool use_default_verify_paths = true;
    };

    // ---------------------------------------------------------------- HTTP
    typedef enum : uint8_t {
        UNKNOWN = 0,
        DEL = 1,
        GET = 2,
        HEAD = 3,
        OPTIONS = 4,
        PATCH = 5,
        POST = 6,
        PUT = 7,
    } request_method_e;

    struct http_request_t {
        request_method_e method = GET;
        std::string path = "/";
        std::string version = "1.1";
        std::map<std::string, std::string> params;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct http_response_t {
        int status_code = 200;
        std::string status_message;
        std::string version = "1.1";
        std::map<std::string, std::string> headers;
        std::string body;
    };

    inline const std::map<int, std::string> response_status_t = {
        {100, "Continue"}, {101, "Switching Protocols"}, {102, "Processing"}, {103, "Early Hints"},
        {200, "OK"}, {201, "Created"}, {202, "Accepted"}, {203, "Non-Authoritative Information"},
        {204, "No Content"}, {205, "Reset Content"}, {206, "Partial Content"}, {207, "Multi-Status"},
        {208, "Already Reported"}, {226, "IM Used"},
        {300, "Multiple Choices"}, {301, "Moved Permanently"}, {302, "Found"}, {303, "See Other"},
        {304, "Not Modified"}, {305, "Use Proxy"}, {306, "Switch Proxy"}, {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},
        {400, "Bad Request"}, {401, "Unauthorized"}, {402, "Payment Required"}, {403, "Forbidden"},
        {404, "Not Found"}, {405, "Method Not Allowed"}, {406, "Not Acceptable"},
        {407, "Proxy Authentication Required"}, {408, "Request Timeout"}, {409, "Conflict"},
        {410, "Gone"}, {411, "Length Required"}, {412, "Precondition Failed"},
        {413, "Payload Too Large"}, {414, "URI Too Long"}, {415, "Unsupported Media Type"},
        {416, "Range Not Satisfiable"}, {417, "Expectation Failed"}, {418, "I'm a teapot"},
        {421, "Misdirected Request"}, {422, "Unprocessable Entity"}, {423, "Locked"},
        {424, "Failed Dependency"}, {425, "Too Early"}, {426, "Upgrade Required"},
        {428, "Precondition Required"}, {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"}, {451, "Unavailable For Legal Reasons"},
        {500, "Internal Server Error"}, {501, "Not Implemented"}, {502, "Bad Gateway"},
        {503, "Service Unavailable"}, {504, "Gateway Timeout"}, {505, "HTTP Version Not Supported"},
        {506, "Variant Also Negotiates"}, {507, "Insufficient Storage"}, {508, "Loop Detected"},
        {510, "Not Extended"}, {511, "Network Authentication Required"}
    };

    inline std::string status_text(int code) {
        auto it = response_status_t.find(code);
        return it != response_status_t.end() ? it->second : std::string("Unknown");
    }

    // ----------------------------------------------------------- WebSocket
    typedef enum : uint8_t {
        CONTINUATION_FRAME = 0x00,
        TEXT_FRAME = 0x01,
        BINARY_FRAME = 0x02,
        CLOSE_FRAME = 0x08,
        PING = 0x09,
        PONG = 0x0A
    } opcode_e;

    typedef enum : uint8_t {
        RSV1 = 0x40,
        RSV2 = 0x20,
        RSV3 = 0x10
    } RSV_e;

    struct dataframe_t {
        bool fin = true;
        bool rsv1 = false;
        bool rsv2 = false;
        bool rsv3 = false;
        bool mask = true;
        opcode_e opcode = TEXT_FRAME;
        size_t length = 0;
        std::array<uint8_t, 4> masking_key{};
    };

    typedef enum : uint8_t {
        CLOSED = 0,
        CLOSING = 1,
        OPEN = 2
    } close_state_e;

    // ------------------------------------------------------------- Options
    struct client_bind_options_t {
        std::string address;
        std::string port = "8080";
        protocol_type_e protocol = v4;
    };

    struct server_bind_options_t {
        std::string address;
        uint16_t port = 8080;
        protocol_type_e protocol = v4;
        bool reuse_address = true;
    };

    struct reconnect_options_t {
        bool enabled = false;
        milliseconds initial_delay{1000};
        milliseconds max_delay{30000};
        double backoff_factor = 2.0;
        unsigned max_attempts = 0; ///< 0 = unlimited
    };

    struct client_options_t {
        milliseconds connect_timeout{10000}; ///< resolve + connect + TLS handshake (0 = none)
        milliseconds idle_timeout{0};        ///< no traffic at all => close (0 = disabled)
        bool tcp_no_delay = true;
        bool keep_alive = true;              ///< SO_KEEPALIVE
        std::size_t read_buffer_size = 16384;
        std::size_t max_write_queue_bytes = 64u * 1024u * 1024u;
        reconnect_options_t reconnect;
    };

    struct server_options_t {
        milliseconds handshake_timeout{10000}; ///< TLS handshake / protocol upgrade
        milliseconds idle_timeout{0};
        bool tcp_no_delay = true;
        bool keep_alive = true;
        std::size_t read_buffer_size = 16384;
        std::size_t max_write_queue_bytes = 64u * 1024u * 1024u;
        std::size_t max_connections = 0; ///< 0 = unlimited
    };

    struct http_limits_t {
        std::size_t max_header_bytes = 64u * 1024u;
        std::size_t max_body_bytes = 16u * 1024u * 1024u;
    };
}
