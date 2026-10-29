# TCP Examples

## TCP Server

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    tcp_server_c net;

    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_client_accepted = [&](const std::shared_ptr<tcp_remote_c> &client) {
        client->on_message_received = [&, client](const std::vector<uint8_t> &buffer, const size_t bytes_recvd) {
            std::cout << client->local_endpoint().port() << " -> " << buffer_to_string(buffer) << std::endl;
        };
        client->on_close = [&, client]() {
            std::cout << "client logout" << std::endl;
        };
    };
    net.open();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
        for (const auto &client: net.clients()) {
            client->write(input, [](const asio::error_code &ec, size_t bytes_sent) {
                // Check if message has been sent
            });
        }
    }
    join_threads();

    return 0;
}
```

## TCP Client

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    tcp_client_c net;

    net.on_error = [&](const asio::error_code &ec) {
        std::cout << ec.message() << std::endl;
    };
    net.on_message_received = [&](const std::vector<uint8_t> &buffer, const size_t bytes_recv) {
        std::cout << buffer_to_string(buffer) << std::endl;
    };
    net.connect();

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
        net.write(input, [](const asio::error_code &ec, size_t bytes_sent) {
            // Check if message has been sent
        });
    }
    join_threads();

    return 0;
}
```
