# TLS Example

```cpp
#define ENABLE_SSL
#include <fstream>
#include <iostream>
#include <iostream>
#include "ip.hpp"

using namespace internetprotocol;

std::string loadfile(const std::string& file) {
    std::ifstream arquivo(file);

    if (!arquivo.is_open()) {
        throw std::runtime_error("Erro trying to open file: " + file);
    }

    std::stringstream buffer;
    buffer << arquivo.rdbuf();

    return buffer.str();
}
// Relative path to the files
const std::string cert = loadfile("cert.pem");
const std::string key = loadfile("key.pem");
const std::string csr = loadfile("csr.pem");
const std::string ca_cert = loadfile("ca-cert.pem");

int main(int argc, char** argv) {
    tcp_server_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
    
    http_server_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
    
    ws_server_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
    
    tcp_client_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
    
    http_client_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
    
    ws_client_ssl_c net({ key, cert, "", "", file_format_e::pem, none, "" });
}

```