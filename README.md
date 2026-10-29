# InternetProtocol

Abstração simples e segura de **UDP, TCP, HTTP e WebSocket** (com TLS opcional) sobre o
**ASIO standalone 1.38.2** (header-only, sem Boost) para aplicações C++20 e Unreal Engine.

* Header-only — o único requisito é um compilador C++20 (MSVC, Clang, GCC) em Windows, Linux ou macOS.
* ASIO 1.38.2 já vem em `thirdparty/asio`.
* TLS opcional (`-DIP_ENABLE_SSL=ON`, requer OpenSSL).

## Build (CMake ≥ 3.24)

```bash
cmake -S . -B build -DIP_ENABLE_SSL=OFF      # ON para habilitar tcp/http/ws *_ssl_c
cmake --build build
ctest --test-dir build --output-on-failure
```

| Opção CMake | Padrão | Descrição |
|---|---|---|
| `IP_ENABLE_SSL` | OFF | Habilita as classes TLS (OpenSSL) |
| `IP_BUILD_TESTS` | ON (top-level) | Compila a suíte de testes |
| `IP_BUILD_EXAMPLES` | ON (top-level) | Compila `examples/src/main.cpp` |
| `IP_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `IP_ENABLE_SANITIZERS` | OFF | ASan + UBSan (GCC/Clang) |

Uso em outro projeto:

```cmake
add_subdirectory(InternetProtocol)
target_link_libraries(meu_app PRIVATE InternetProtocol::InternetProtocol)
```

## Arquitetura

```
include/ip.hpp                umbrella
include/ip/core/              config, error (std::error_code), runtime (io_context + pool), types
include/ip/events/            event<> – callbacks thread-safe (on_xxx = lambda)
include/ip/transport/         connection (fila de escrita, strand), stream_client, stream_server, remote
include/ip/utils/             string, package, net (HTTP), httpparser, dataframe/handshake (WS), tls
include/ip/{udp,tcp,http,websocket}/   APIs públicas
```

Princípios: **um único `io_context`** com pool fixo de threads; cada conexão tem sua **strand**
(sem locks no caminho de I/O); todo handler assíncrono captura `shared_from_this()`/`weak_ptr`
(nunca `this` cru ou `[&]`); buffers de escrita pertencem à fila (nunca dangling); fila de saída
com limite (back-pressure); `close()` é gracioso (descarrega a fila) e `abort()` é imediato.

## Exemplo

```cpp
#include "ip.hpp"
using namespace internetprotocol;

ws_client_c ws;
ws.options().reconnect.enabled = true;        // reconexão com backoff exponencial
ws.set_ping_interval(15);                     // keep-alive + detecção de peer mudo
ws.on_connected        = [&](const http_response_t &) { ws.write("hello"); };
ws.on_message_received = [](const std::vector<uint8_t> &data, bool binary) { /* ... */ };
ws.on_error            = [](const asio::error_code &ec) { /* ec.message() */ };
ws.connect({"localhost", "8080", v4});
```

Mais exemplos em [`examples/`](examples) (TCP, UDP, HTTP, WS, TLS). Relatório técnico da Fase 1:
[`docs/RELATORIO_FASE1.md`](docs/RELATORIO_FASE1.md).

## Notas de threading

* Callbacks executam em threads do pool (serializados por conexão). Nunca bloqueie dentro deles.
* Todos os métodos públicos (`write`, `close`, `connect`, ...) são thread-safe.
* `on_xxx = ...` pode ser reatribuído a qualquer momento (`event<>` é thread-safe).
* `join_threads()` aguarda o trabalho pendente; `stop_threads()` interrompe imediatamente.
  O runtime reinicia sozinho no próximo `connect()/open()`.

## Licença

MIT. ASIO: Boost Software License 1.0 (`thirdparty/asio/LICENSE_1_0.txt`).
