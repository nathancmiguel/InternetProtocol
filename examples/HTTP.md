# HTTP Examples

## HTTP Server

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    http_server_c net;

    http_request_t req;
    net.get("/", [&](const http_request_t &request, const std::shared_ptr<http_remote_c> &response) {
        http_response_t &res = response->headers();
        res.body = "hello, world!";
        response->write([](const asio::error_code &ec, const size_t bytes_sent) {
            // Check if response has been sent
        });
    });

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
    }
    join_threads();

    return 0;
}
```

## HTTP Client

```cpp
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

int main(int argc, char** argv) {
    http_client_c net;

    net.set_host();

    http_request_t req;
    req.headers.insert_or_assign("Host", "localhost:8080");
    req.headers.insert_or_assign("Connection", "close");
    net.request(req, [&](const asio::error_code &ec, const http_response_t &res) {
        if (ec) {
            // ec will be true if any error occur when trying to connect, send or receive payload
            std::cout << ec.message() << std::endl;
            return;
        }
        std::cout << res.body << std::endl;
    });

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input == "quit") {
            net.close();
            break;
        }
    }
    join_threads();

    return 0;
}
```