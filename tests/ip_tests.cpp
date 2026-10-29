// Self-contained test suite (no external framework). Run through CTest or directly.
#include "ip.hpp"

#include <iostream>

using namespace internetprotocol;
using namespace std::chrono_literals;

static std::atomic<int> g_failures{0};
static std::atomic<int> g_checks{0};

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) { ++g_failures; std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; } \
    } while (0)

template<typename T>
static bool wait(std::future<T> &f, std::chrono::seconds s = 8s) { return f.wait_for(s) == std::future_status::ready; }

static std::string port_of(const tcp::endpoint &e) { return std::to_string(e.port()); }

// ------------------------------------------------------------------ units
static void test_utils() {
    CHECK(trim_whitespace("") == "");
    CHECK(trim_whitespace("   ") == "");
    CHECK(trim_whitespace("  a b \r\n") == "a b");
    CHECK(split_string("a,b,,c", ',').size() == 4);
    std::string s = "AbC";
    string_to_lower(s);
    CHECK(s == "abc");
    CHECK(url_decode(url_encode("a b&c=d/é")) == "a b&c=d/é");
    CHECK(is_valid_utf8(reinterpret_cast<const uint8_t *>("héllo"), 6));
    const uint8_t bad[] = {0xC0, 0x80};
    CHECK(!is_valid_utf8(bad, 2));
    CHECK(slice_string("abcdefg", 3).size() == 3);
    CHECK(slice_buffer({}, 0).empty());
}

static void test_handshake() {
    // RFC 6455 §1.3 sample
    CHECK(generate_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(base64_encode(reinterpret_cast<const uint8_t *>("Man"), 3) == "TWFu");
    CHECK(base64_encode(reinterpret_cast<const uint8_t *>("Ma"), 2) == "TWE=");
    CHECK(generate_websocket_key() != generate_websocket_key());
    http_request_t req;
    req.headers["Connection"] = "keep-alive, Upgrade";
    req.headers["upgrade"] = "WebSocket";
    req.headers["Sec-WebSocket-Key"] = "dGhlIHNhbXBsZSBub25jZQ==";
    req.headers["Sec-WebSocket-Version"] = "13";
    http_response_t res;
    CHECK(validate_handshake_request(req, res));
}

static void test_http_parser() {
    std::error_code ec;
    {
        http_parser p(http_parser::kind::request);
        const std::string raw = "POST /a%20b?x=1&y=h%C3%A9 HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhelloGET /";
        const size_t used = p.feed(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), ec);
        CHECK(!ec && p.done() && used == raw.size() - 5);
        CHECK(p.request().path == "/a b" && p.request().params["y"] == "hé" && p.request().body == "hello");
    }
    {
        http_parser p(http_parser::kind::response);
        const std::string raw = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nX-T: 1\r\n\r\n";
        // feed byte by byte: incremental parsing must be framing-agnostic
        for (char c: raw) p.feed(reinterpret_cast<const uint8_t *>(&c), 1, ec);
        CHECK(!ec && p.done() && p.response().body == "Wikipedia" && p.response().headers["x-t"] == "1");
    }
    {
        http_parser p(http_parser::kind::request); // smuggling shape must be rejected
        const std::string raw = "POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n";
        p.feed(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), ec);
        CHECK(ec == errc::protocol_error);
    }
    {
        http_parser p(http_parser::kind::request, http_limits_t{128, 16});
        ec.clear();
        const std::string raw = "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n";
        p.feed(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), ec);
        CHECK(ec == errc::message_too_big);
    }
    {
        http_request_t r; // CR/LF injection must be neutralised
        r.headers["X-A"] = "v\r\nInjected: 1";
        const std::string out = prepare_request(r, "h", 80);
        CHECK(out.find("\r\nInjected:") == std::string::npos);
    }
}

static void test_ws_frames() {
    dataframe_t df;
    df.mask = true;
    const std::string msg(70000, 'x');
    auto frame = encode_frame(reinterpret_cast<const uint8_t *>(msg.data()), msg.size(), df);
    std::vector<uint8_t> payload;
    dataframe_t out;
    CHECK(decode_payload(frame, payload, out) && payload.size() == msg.size() && out.mask);
    ws_frame_parser p(1u << 20, true); // server side: client frames must be masked
    size_t frames = 0;
    uint16_t err = 0;
    for (uint8_t b: frame) err |= p.feed(&b, 1, [&](ws_frame &&f) { ++frames; CHECK(f.payload.size() == msg.size()); });
    CHECK(err == 0 && frames == 1);
    // unmasked client frame, RSV bit and oversize are protocol errors
    df.mask = false;
    auto unmasked = encode_frame(reinterpret_cast<const uint8_t *>("a"), 1, df);
    ws_frame_parser srv(1024, true);
    CHECK(srv.feed(unmasked.data(), unmasked.size(), [](ws_frame &&) {}) == 1002);
    ws_frame_parser small(10, false);
    auto big = encode_frame(reinterpret_cast<const uint8_t *>(msg.data()), 100, df);
    CHECK(small.feed(big.data(), big.size(), [](ws_frame &&) {}) == 1009);
    CHECK(decode_payload({0x81}, payload, out) == false); // truncated
}

static void test_event() {
    event<void(int)> e;
    int v = 0;
    CHECK(!e);
    e = [&](int x) { v = x; };
    e(42);
    CHECK(v == 42 && e);
    e = [](int) { throw std::runtime_error("contained"); };
    e(1); // must not throw
    e = nullptr;
    CHECK(!e);
}

// ------------------------------------------------------------ integration
static void test_tcp() {
    tcp_server_c server;
    tcp_client_c client;
    std::atomic<int> accepted{0};
    server.on_client_accepted = [&](const std::shared_ptr<tcp_remote_c> &c) {
        ++accepted;
        c->on_message_received = [w = std::weak_ptr<tcp_remote_c>(c)](const std::vector<uint8_t> &b, size_t) {
            if (auto s = w.lock()) s->write_buffer(b);
        };
    };
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    CHECK(!server.open({"127.0.0.1", 0, v4, true})); // already open

    std::promise<std::string> got;
    std::string acc;
    const std::string big(1 << 20, 'q'); // 1 MiB: exercises the write queue + partial reads
    client.on_connected = [&] { CHECK(client.write(big)); };
    client.on_message_received = [&](const std::vector<uint8_t> &b, size_t n) {
        acc.append(b.begin(), b.begin() + n);
        if (acc.size() == big.size()) got.set_value(acc);
    };
    CHECK(client.connect({"127.0.0.1", port_of(server.local_endpoint()), v4}));
    CHECK(!client.connect({"127.0.0.1", "1", v4})); // already connecting
    auto f = got.get_future();
    CHECK(wait(f) && f.get() == big);
    CHECK(server.clients().size() == 1);

    std::promise<void> closed;
    client.on_close = [&] { closed.set_value(); };
    client.close();
    auto fc = closed.get_future();
    CHECK(wait(fc));
    CHECK(!client.write("x")); // closed
    server.close();
}

static void test_many_clients() {
    // Regression: the old design blocked one pool thread per object and starved the (N+1)th one.
    tcp_server_c server;
    std::atomic<int> msgs{0};
    server.on_client_accepted = [&](const std::shared_ptr<tcp_remote_c> &c) {
        c->on_message_received = [&](const std::vector<uint8_t> &, size_t) { ++msgs; };
    };
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    constexpr int N = 40;
    std::vector<std::unique_ptr<tcp_client_c>> clients;
    for (int i = 0; i < N; ++i) {
        auto c = std::make_unique<tcp_client_c>();
        tcp_client_c *raw = c.get();
        c->on_connected = [raw] { raw->write("hi"); };
        CHECK(c->connect({"127.0.0.1", port_of(server.local_endpoint()), v4}));
        clients.push_back(std::move(c));
    }
    for (int i = 0; i < 100 && msgs < N; ++i) std::this_thread::sleep_for(50ms);
    CHECK(msgs == N);
    clients.clear(); // destroying while connected must be safe
    server.close();
}

static void test_tcp_failures() {
    tcp_client_c c;
    std::promise<std::error_code> p;
    c.options().connect_timeout = 2s;
    c.on_error = [&](const asio::error_code &ec) { try { p.set_value(ec); } catch (...) {} };
    CHECK(c.connect({"127.0.0.1", "1", v4})); // nothing listens on port 1
    auto f = p.get_future();
    CHECK(wait(f) && f.get());
    CHECK(!c.is_open());
    tcp_client_c d;
    CHECK(!d.connect({"", "80", v4})); // invalid argument
}

static void test_reconnect() {
    tcp_server_c server;
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    const auto port = server.local_endpoint().port();
    tcp_client_c c;
    c.options().reconnect.enabled = true;
    c.options().reconnect.initial_delay = 100ms;
    std::atomic<int> connects{0};
    c.on_connected = [&] { ++connects; };
    CHECK(c.connect({"127.0.0.1", std::to_string(port), v4}));
    for (int i = 0; i < 100 && connects < 1; ++i) std::this_thread::sleep_for(20ms);
    CHECK(connects == 1);
    server.close(); // drops the connection -> client must come back once the server is back
    server.on_listening = [] {};
    std::this_thread::sleep_for(200ms);
    CHECK(server.open({"127.0.0.1", port, v4, true}));
    for (int i = 0; i < 150 && connects < 2; ++i) std::this_thread::sleep_for(20ms);
    CHECK(connects >= 2);
    c.close();
    server.close();
}

static void test_udp() {
    udp_server_c server;
    udp_client_c client;
    server.on_message_received = [&](const std::vector<uint8_t> &b, size_t n, const udp::endpoint &ep) {
        server.send_to("pong:" + std::string(b.begin(), b.begin() + n), ep);
    };
    CHECK(server.bind({"127.0.0.1", 0, v4, true}));
    std::promise<std::string> got;
    client.on_message_received = [&](const std::vector<uint8_t> &b, size_t n, const udp::endpoint &) {
        try { got.set_value(std::string(b.begin(), b.begin() + n)); } catch (...) {}
    };
    client.on_connected = [&] { client.send("ping"); };
    CHECK(client.connect({"127.0.0.1", std::to_string(server.local_endpoint().port()), v4}));
    auto f = got.get_future();
    CHECK(wait(f) && f.get() == "pong:ping");
    CHECK(client.remote_endpoint().port() == server.local_endpoint().port()); // destination never overwritten
    client.close();
    server.close();
    CHECK(!client.is_open() && !server.is_open());
}

static void test_http() {
    http_server_c server;
    http_client_c client;
    server.get("/hello", [](const http_request_t &req, const std::shared_ptr<http_remote_c> &r) {
        r->response.body = "hello " + (req.params.count("q") ? req.params.at("q") : std::string("-"));
        r->write();
    });
    server.post("/echo", [](const http_request_t &req, const std::shared_ptr<http_remote_c> &r) {
        r->response.body = req.body;
        r->write();
    });
    server.all("/any", [](const http_request_t &, const std::shared_ptr<http_remote_c> &r) {
        // asynchronous response (from another thread)
        std::thread([r] { std::this_thread::sleep_for(50ms); r->response.body = "late"; r->write(); }).detach();
    });
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    client.set_host({"127.0.0.1", port_of(server.local_endpoint()), v4});

    struct R { std::error_code ec; http_response_t res; };
    std::vector<std::promise<R>> ps(6);
    auto submit = [&](int i, http_request_t req) {
        client.request(req, [&, i](const asio::error_code &ec, const http_response_t &res) { ps[i].set_value({ec, res}); });
    };
    http_request_t a; a.path = "/hello"; a.params["q"] = "x y";
    http_request_t b; b.method = POST; b.path = "/echo"; b.body = std::string(500000, 'z');
    http_request_t c; c.path = "/missing";
    http_request_t d; d.method = PUT; d.path = "/hello";
    http_request_t e; e.path = "/any";
    http_request_t h; h.method = HEAD; h.path = "/hello";
    submit(0, a); submit(1, b); submit(2, c); submit(3, d); submit(4, e); submit(5, h);
    std::vector<R> r;
    for (auto &p: ps) { auto f = p.get_future(); CHECK(wait(f)); r.push_back(f.get()); }
    CHECK(!r[0].ec && r[0].res.status_code == 200 && r[0].res.body == "hello x y");
    CHECK(r[1].res.body.size() == 500000);
    CHECK(r[2].res.status_code == 404);
    CHECK(r[3].res.status_code == 405 && r[3].res.headers.count("allow"));
    CHECK(r[4].res.body == "late");
    CHECK(r[5].res.status_code == 200 && r[5].res.body.empty()); // HEAD: headers only, still parsed
    client.close();
    server.close();
}

static void test_http_malformed() {
    http_server_c server;
    server.get("/", [](const http_request_t &, const std::shared_ptr<http_remote_c> &r) { r->write(); });
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    tcp_client_c raw;
    std::promise<std::string> got;
    std::string acc;
    raw.on_connected = [&] { raw.write("GARBAGE\r\n\r\n"); };
    raw.on_message_received = [&](const std::vector<uint8_t> &b, size_t n) { acc.append(b.begin(), b.begin() + n); };
    std::promise<void> closed;
    raw.on_close = [&] { closed.set_value(); };
    raw.connect({"127.0.0.1", port_of(server.local_endpoint()), v4});
    auto fc = closed.get_future();
    CHECK(wait(fc));
    CHECK(acc.rfind("HTTP/1.1 400", 0) == 0);
    server.close();
}

static void test_websocket() {
    ws_server_c server;
    ws_client_c client;
    std::promise<std::pair<uint16_t, std::string>> server_closed;
    server.on_client_accepted = [&](const std::shared_ptr<ws_remote_c> &c) {
        c->on_message_received = [w = std::weak_ptr<ws_remote_c>(c)](const std::vector<uint8_t> &b, bool bin) {
            if (auto s = w.lock()) { if (bin) s->write_buffer(b); else s->write("echo:" + buffer_to_string(b)); }
        };
        c->on_close = [&](uint16_t code, const std::string &reason) { try { server_closed.set_value({code, reason}); } catch (...) {} };
    };
    CHECK(server.open({"127.0.0.1", 0, v4, true}));

    std::promise<std::string> text;
    std::promise<size_t> bin;
    std::promise<void> pong;
    client.on_connected = [&](const http_response_t &r) { CHECK(r.status_code == 101); client.write("hello"); };
    client.on_message_received = [&](const std::vector<uint8_t> &b, bool is_bin) {
        try { if (is_bin) bin.set_value(b.size()); else text.set_value(buffer_to_string(b)); } catch (...) {}
    };
    client.on_pong = [&] { try { pong.set_value(); } catch (...) {} };
    std::promise<std::pair<uint16_t, std::string>> client_closed;
    client.on_close = [&](uint16_t c, const std::string &r) { client_closed.set_value({c, r}); };
    CHECK(client.connect({"127.0.0.1", port_of(server.local_endpoint()), v4}));

    auto ft = text.get_future();
    CHECK(wait(ft) && ft.get() == "echo:hello");
    auto fb = bin.get_future();
    CHECK(client.write_buffer(std::vector<uint8_t>(400000, 9)));
    CHECK(wait(fb) && fb.get() == 400000);
    auto fp = pong.get_future();
    CHECK(client.ping());
    CHECK(wait(fp));
    CHECK(client.is_open());

    client.end(1000, "bye");
    auto fc = client_closed.get_future();
    CHECK(wait(fc));
    auto cc = fc.get();
    CHECK(cc.first == 1000);
    auto fs = server_closed.get_future();
    CHECK(wait(fs));
    CHECK(fs.get().first == 1000);
    CHECK(!client.is_open());
    CHECK(!client.write("late"));
    server.close();
}

static void test_websocket_invalid_peer() {
    // Plain HTTP client talking to a ws server -> 400/426, no crash.
    ws_server_c server;
    CHECK(server.open({"127.0.0.1", 0, v4, true}));
    http_client_c http;
    http.set_host({"127.0.0.1", port_of(server.local_endpoint()), v4});
    std::promise<int> p;
    http_request_t req;
    http.request(req, [&](const asio::error_code &, const http_response_t &r) { p.set_value(r.status_code); });
    auto f = p.get_future();
    CHECK(wait(f) && f.get() >= 400);
    http.close();
    server.close();
}

int main() {
    test_utils();
    test_handshake();
    test_http_parser();
    test_ws_frames();
    test_event();
    test_tcp();
    test_many_clients();
    test_tcp_failures();
    test_reconnect();
    test_udp();
    test_http();
    test_http_malformed();
    test_websocket();
    test_websocket_invalid_peer();
    stop_threads();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
