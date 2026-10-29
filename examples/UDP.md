# UDP Examples

## UDP Server

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    udp_server_c net;

    std::set<udp::endpoint> endpoints{};
    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_message_received = [&](const std::vector<uint8_t> &buffer, const size_t bytes_recvd, const udp::endpoint &endpoint) {
        if (endpoints.find(endpoint) == endpoints.end()) {
            endpoints.insert(endpoint);
        }
        std::string message = buffer_to_string(buffer);
        for (const auto &ep: endpoints) {
            net.send_to(message, ep); // Broadcast message to other clients
        }
    };
    net.bind();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
        for (const auto &ep: endpoints) {
            net.send_to(input, ep, [](const asio::error_code &ec, size_t bytes_sent, const udp::endpoint &endpoint) {
                // Check if message has been sent
            });
        }
    }
    join_threads();

    return 0;
}

```

## UDP Client

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    udp_client_c net;

    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_message_received = [&](const std::vector<uint8_t> &buffer, const size_t bytes_recvd, const udp::endpoint &endpoint) {
        std::string message = buffer_to_string(buffer);
    };
    net.connect();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
        net.send(input, [](const asio::error_code &ec, const size_t bytes_sent) {
            // Check if message has been sent
        });
    }
    join_threads();

    return 0;
}

```