// Minimal end-to-end demo: WebSocket echo server + client in one process.
#include "ip.hpp"

#include <iostream>

using namespace internetprotocol;

int main() {
    ws_server_c server;
    server.on_error = [](const asio::error_code &ec) { std::cout << "server error: " << ec.message() << "\n"; };
    server.on_client_accepted = [](const std::shared_ptr<ws_remote_c> &client) {
        std::cout << "client accepted\n";
        client->on_message_received = [w = std::weak_ptr<ws_remote_c>(client)](const std::vector<uint8_t> &data, bool binary) {
            if (auto c = w.lock()) binary ? c->write_buffer(data) : c->write("echo: " + buffer_to_string(data));
        };
        client->on_close = [](const uint16_t code, const std::string &reason) { std::cout << "client closed " << code << " " << reason << "\n"; };
    };
    if (!server.open({"127.0.0.1", 8080, v4, true})) return 1;

    ws_client_c client;
    std::promise<void> done;
    client.options().reconnect.enabled = false;
    client.set_ping_interval(15);
    client.on_connected = [&](const http_response_t &) { client.write("Hello, WebSocket!"); };
    client.on_message_received = [&](const std::vector<uint8_t> &data, bool) {
        std::cout << buffer_to_string(data) << "\n";
        client.end(1000, "done");
    };
    client.on_close = [&](const uint16_t code, const std::string &) { std::cout << "closed " << code << "\n"; done.set_value(); };
    client.on_error = [](const asio::error_code &ec) { std::cout << "client error: " << ec.message() << "\n"; };
    client.connect({"127.0.0.1", "8080", v4});

    done.get_future().wait_for(std::chrono::seconds(10));
    server.close();
    join_threads();
    return 0;
}
