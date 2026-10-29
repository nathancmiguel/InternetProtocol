// TLS tests (wss / https / tcp+tls). The certificate is generated in memory at start-up.
#include "ip.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <iostream>

using namespace internetprotocol;
using namespace std::chrono_literals;

static std::atomic<int> g_failures{0};
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; } } while (0)

static void make_cert(std::string &cert_pem, std::string &key_pem) {
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    X509 *x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -60);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    X509_add_ext(x, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x, pkey, EVP_sha256());
    auto grab = [](auto writer) {
        BIO *b = BIO_new(BIO_s_mem());
        writer(b);
        char *p = nullptr;
        const long n = BIO_get_mem_data(b, &p);
        std::string s(p, static_cast<size_t>(n));
        BIO_free(b);
        return s;
    };
    cert_pem = grab([&](BIO *b) { PEM_write_bio_X509(b, x); });
    key_pem = grab([&](BIO *b) { PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr); });
    X509_free(x);
    EVP_PKEY_free(pkey);
}

int main() {
    std::string cert, key;
    make_cert(cert, key);
    security_context_opts srv_opts;
    srv_opts.cert = cert;
    srv_opts.private_key = key;
    srv_opts.verify_mode = none;
    security_context_opts trusted;
    trusted.ca_cert = cert;
    trusted.verify_mode = verify_peer;
    trusted.host_name_verification = "localhost";

    { // tcp + tls echo, large payload
        tcp_server_ssl_c server(srv_opts);
        server.on_client_accepted = [](const std::shared_ptr<tcp_remote_ssl_c> &c) {
            c->on_message_received = [w = std::weak_ptr<tcp_remote_ssl_c>(c)](const std::vector<uint8_t> &b, size_t) {
                if (auto s = w.lock()) s->write_buffer(b);
            };
        };
        CHECK(server.open({"127.0.0.1", 0, v4, true}));
        tcp_client_ssl_c client(trusted);
        const std::string payload(300000, 'k');
        std::string acc;
        std::promise<void> done;
        client.on_connected = [&] { client.write(payload); };
        client.on_message_received = [&](const std::vector<uint8_t> &b, size_t n) {
            acc.append(b.begin(), b.begin() + n);
            if (acc.size() == payload.size()) done.set_value();
        };
        CHECK(client.connect({"127.0.0.1", std::to_string(server.local_endpoint().port()), v4}));
        auto f = done.get_future();
        CHECK(f.wait_for(10s) == std::future_status::ready && acc == payload);
        client.close();
        server.close();
    }
    { // https: trusted CA works, unknown CA is rejected
        http_server_ssl_c server(srv_opts);
        server.get("/", [](const http_request_t &, const std::shared_ptr<http_remote_ssl_c> &r) { r->response.body = "secure"; r->write(); });
        CHECK(server.open({"127.0.0.1", 0, v4, true}));
        const auto port = std::to_string(server.local_endpoint().port());

        http_client_ssl_c good(trusted);
        good.set_host({"127.0.0.1", port, v4});
        std::promise<std::string> p1;
        good.request({}, [&](const asio::error_code &ec, const http_response_t &r) { p1.set_value(ec ? ec.message() : r.body); });
        auto f1 = p1.get_future();
        CHECK(f1.wait_for(10s) == std::future_status::ready && f1.get() == "secure");

        security_context_opts untrusted;
        untrusted.verify_mode = verify_peer;
        http_client_ssl_c bad(untrusted);
        bad.set_host({"127.0.0.1", port, v4});
        std::promise<std::error_code> p2;
        bad.request({}, [&](const asio::error_code &ec, const http_response_t &) { p2.set_value(ec); });
        auto f2 = p2.get_future();
        CHECK(f2.wait_for(10s) == std::future_status::ready && f2.get());
        good.close();
        server.close();
    }
    { // wss
        ws_server_ssl_c server(srv_opts);
        server.on_client_accepted = [](const std::shared_ptr<ws_remote_ssl_c> &c) {
            c->on_message_received = [w = std::weak_ptr<ws_remote_ssl_c>(c)](const std::vector<uint8_t> &b, bool) {
                if (auto s = w.lock()) s->write("tls:" + buffer_to_string(b));
            };
        };
        CHECK(server.open({"127.0.0.1", 0, v4, true}));
        ws_client_ssl_c client(trusted);
        std::promise<std::string> got;
        std::promise<void> closed;
        client.on_connected = [&](const http_response_t &) { client.write("x"); };
        client.on_message_received = [&](const std::vector<uint8_t> &b, bool) { got.set_value(buffer_to_string(b)); };
        client.on_close = [&](uint16_t, const std::string &) { closed.set_value(); };
        CHECK(client.connect({"127.0.0.1", std::to_string(server.local_endpoint().port()), v4}));
        auto f = got.get_future();
        CHECK(f.wait_for(10s) == std::future_status::ready && f.get() == "tls:x");
        client.end();
        auto fc = closed.get_future();
        CHECK(fc.wait_for(10s) == std::future_status::ready);
        server.close();
    }
    stop_threads();
    std::cout << (g_failures == 0 ? "TLS tests passed\n" : "TLS tests FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
