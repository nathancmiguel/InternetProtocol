# WS Examples

## WS Server

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    ws_server_c net;

    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_client_accepted = [&](const std::shared_ptr<ws_remote_c> &client) {
        std::cout << "Client connected" << std::endl;
        client->on_error = [&](const asio::error_code &ec) {
            std::cout << ec.message() << std::endl;
        };
        client->on_close = [&](const uint16_t code, const std::string &reason) {
            std::cout << code << " -> " << reason << std::endl;
        };
        client->on_message_received = [&](const std::vector<uint8_t> &buffer, bool is_binary) {
            std::cout << "Message received: " << buffer_to_string(buffer) << std::endl;
        };
    };
    net.open();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
        for (const auto &client : net.clients()) {
            client->write(input, {}, [](const asio::error_code &ec, const size_t bytes_sent) {
                // Check if message has been sent
            });
        }
    }
    join_threads();

    return 0;
}
```

## WS Client

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    ws_client_c net;

    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_message_received = [&](const std::vector<uint8_t> &buffer, bool is_binary) {
        std::cout << buffer_to_string(buffer) << std::endl;
    };
    net.connect();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close(1000, "Shutdown client");
            break;
        }
        net.write(input, {}, [](const asio::error_code &ec, size_t bytes_sent) {
            // Check if message has been sent
        });
    }
    join_threads();

    return 0;
}
```