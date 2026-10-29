# Relatório Técnico — Fase 1 (InternetProtocol-master)

Base: ASIO standalone **1.38.2** (`ASIO_VERSION 103802`), C++20, CMake ≥ 3.24, sem Boost.

## 1. Arquivos

**Removidos:** `xmake.lua`, `src/main.cpp` (substituído por `examples/src/main.cpp`).

**Criados:** `CMakeLists.txt`, `thirdparty/CMakeLists.txt`, `thirdparty/asio/**` (ASIO 1.38.2), `examples/CMakeLists.txt`,
`examples/src/main.cpp`, `tests/{CMakeLists.txt,ip_tests.cpp,tls_tests.cpp}`, `docs/RELATORIO_FASE1.md`,
`include/ip/core/{config,error,runtime,types}.hpp`, `include/ip/events/event.hpp`,
`include/ip/transport/{connection,remote,stream_client,stream_server}.hpp`,
`include/ip/utils/{httpparser,string,tls}.hpp`, `include/ip/websocket/wsengine.hpp`.

**Reescritos/modificados:** `.gitignore`, `README.md`, `examples/*.md`, `include/ip.hpp`, `include/ip/net/common.hpp` (agora shim),
`utils/{buffer,dataframe,handshake,net,package}.hpp`, `tcp/{tcpclient,tcpremote,tcpserver}.hpp`,
`udp/{udpclient,udpserver}.hpp`, `http/{httpclient,httpremote,httpserver}.hpp`, `websocket/{wsclient,wsremote,wsserver}.hpp`.

## 2. Bugs encontrados → correção

### Concorrência / lifetime / UB (críticos)
| # | Problema | Correção |
|---|---|---|
| 1 | Cada client/server tinha **seu próprio `io_context`** e ocupava 1 thread do pool com `run()` bloqueante: o (N+1)º objeto (N = nº de cores) nunca executava. | `core/runtime.hpp`: **um** `io_context` + pool fixo; cada conexão usa uma **strand**. Teste `test_many_clients` (40 clientes). |
| 2 | Handlers com `[&]`/`this` cru: use-after-free quando o objeto era destruído com operações pendentes. | Todo handler captura `shared_from_this()` ou `weak_ptr`; facades possuem o estado via `shared_ptr`, destrutor chama `shutdown()`. |
| 3 | `async_write` com buffer local/`payload` que morria antes do fim da operação (dangling). | Fila de escrita própria (`connection::wq_`) — os bytes pertencem à conexão até a conclusão. |
| 4 | Múltiplos `async_write` concorrentes intercalavam bytes (sem fila). | Escrita serializada com *gather write* (até 16 msgs/256 KiB por syscall). |
| 5 | `close()` chamava `context.stop()/restart()`/`join` de dentro de handlers e invocava callbacks sob mutex → **deadlock**. | `close()` graceful/`abort()` postados na strand; callbacks nunca rodam sob lock (`event<>` copia o handler e chama fora do lock). |
| 6 | `std::function` públicos (`on_*`) lidos pelas threads de rede enquanto o usuário os reatribuía → **data race**. | `events/event.hpp`: handler imutável em `shared_ptr` + lock curto; mesma sintaxe `obj.on_x = lambda`; exceções do usuário contidas. |
| 7 | `server.clients()` devolvia `const std::set&` mutável por threads de rede (invalidação de iterador). | Snapshot `std::vector<shared_ptr<Remote>>` sob mutex. |
| 8 | Dados perdidos: `close()` descartava a fila de saída. | `close()` descarrega a fila antes (TLS: `close_notify` com timeout). |
| 9 | UDP client sobrescrevia o endpoint de destino com o do remetente (`receive_from`). | `async_receive_from` com endpoint próprio; destino imutável. |
| 10 | `std::random_device` + `mt19937` construídos **a cada frame** WS. | `thread_local` semeado uma vez. |
| 11 | SHA-1: variáveis globais não usadas e `uint8_t << 24` (overflow de `int` = UB). | SHA-1 sem estado global, aritmética `uint32_t`; vetor do RFC 6455 testado. |
| 12 | `trim_whitespace` com string vazia (UB), `isspace(char)` negativo (UB); `slice_*` devolviam views de temporário (dangling) e `while (offset < size)` errado; divisão por zero com `size==0`. | Reescritos (views de origem, guardas). |
| 13 | Funções/objetos não-`inline` em headers → ODR/duplicate symbols; `using namespace asio::ip` global; `#define ASIO_NOEXCEPT` vazio. | Tudo `inline`; `using` apenas dentro de `internetprotocol`. |

### TCP
Sem timeout de conexão, sem keep-alive/TCP_NODELAY, reconnect inexistente, erro de leitura tratado de forma inconsistente.
→ `connect_timeout`, `idle_timeout`, `SO_KEEPALIVE`, `TCP_NODELAY`, **reconnect com backoff exponencial** (`options().reconnect`),
limite de fila (`queue_full`), resolve filtrado por família (v4/v6/unspecified), `get_error_code()` thread-safe.

### UDP
Bind/opções com overloads que lançam exceção; sem multicast/broadcast úteis. → `bind()` sem exceções, `set_broadcast`,
`join/leave_multicast_group`, `set_multicast_options`, buffer de recepção reutilizado (sem alocação por datagrama), envio com buffer dono.

### HTTP
* Parser ad-hoc → `utils/httpparser.hpp`: incremental, **Content-Length, chunked (+trailers), read-until-close**, limites de header/body
  (431/413), rejeita *request smuggling* (CL+TE, CL duplicado/inválido), `1xx` intermediários, `HEAD`/`204`/`304`.
* `Content-Length` calculado com condição `||` errada; sem sanitização → **header injection** (CR/LF). Agora sanitizado.
* `Host` usava o IP remoto em vez do host pedido. → usa o host configurado.
* Servidor: sem 404 (a conexão ficava pendurada), `keep-alive` invertido (fechava em HTTP/1.1), caminho incluía `?query`.
  → 404/405(`Allow`)/501/505/400, `HEAD`→handler `GET`, query em `params` (URL-decodificada), respostas **ordenadas** com pipelining,
  rotas protegidas por mutex, exceção em handler → 500.
* Cliente: fila de requisições em ordem, reuso de conexão, retry único de requisições idempotentes em conexão keep-alive obsoleta,
  timeout por requisição (`set_request_timeout`), `idle_timeout_seconds`.

### WebSocket
* `Sec-WebSocket-Key` era **constante** (exemplo do RFC) → nonce aleatório por conexão.
* Handshake case-sensitive (`Connection: keep-alive, Upgrade` falhava) → comparação por token, case-insensitive.
* Sem `CONTINUATION`, sem validação de RSV/opcodes/tamanho de controle, sem UTF-8, sem limites; `reserve(len)` com tamanho do atacante (DoS).
  → `wsengine.hpp`: fragmentação completa, RSV/opcode/controle (≤125, FIN), máscara obrigatória cliente→servidor e proibida servidor→cliente,
  UTF-8 estrito (1007), `max_message_bytes` (1009), códigos de close válidos, eco de CLOSE, `end()` (handshake com timeout) e `close()`.
* Ping/pong automáticos + **heartbeat** (`set_ping_interval`; peer mudo é derrubado), reconnect integrado.
* Estado de fechamento (`CLOSED/CLOSING/OPEN`) atômico e consistente.

### TLS
`verify_peer` sem carregar CAs (sempre falhava) e hostname/SNI ausentes → `ca_cert`, store do sistema, SNI e `host_name_verification`;
TLS ≥ 1.2; erros de certificado viram `error_code` (sem exceção). Teste valida CA confiável × não confiável.

## 3. Mudanças de API (documentadas)
* `server.clients()` retorna **snapshot** (`std::vector`), não referência.
* `close()` é **graceful**; use `abort()` para imediato. `http remote->headers()` continua funcionando (`response` é o membro novo).
* `all()` agora é *fallback* (rota específica tem prioridade) em vez de executar antes de toda rota.
* `security_context_opts`: campos novos (`ca_cert`, `use_default_verify_paths`) ao **final** — a inicialização posicional antiga continua válida.
* Tipos internos removidos (`tcp_client_t`, etc.) e o `std::function` público virou `event<>` (mesma sintaxe de atribuição).
* Novos: `client_options_t`/`server_options_t`, `errc` (erros em `std::error_code`), `protocol_type_e::unspecified`.
Todos os exemplos `.md` originais compilam sem alteração de código (apenas `#include <iostream>` explícito).

## 4. Verificação realizada
GCC 13.3 / Ubuntu 24.04, C++20:
* Build Release + OpenSSL, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`: **sem warnings**.
* `ctest`: **128 checks (ip_tests) + testes TLS**, 100 % OK.
* **ASan + UBSan**: limpo (3 execuções). **ThreadSanitizer**: 1 corrida real encontrada
  (`internal_on_closed` zerado durante leitura) e corrigida; depois **5 execuções limpas**.
* Exemplos originais dos `.md` compilados contra a nova API.

**Não verificado neste ambiente:** MSVC, Clang e macOS (código segue C++20 padrão e as macros de plataforma estão no CMake, mas não foi compilado lá).

## 5. Limitações conhecidas
HTTP: sem redirects, `Expect: 100-continue`, compressão ou HTTP/2. WebSocket: sem `permessage-deflate` (RSV1 é rejeitado).
Callbacks rodam em threads do pool — na Fase 2 serão roteados para a Game Thread no plugin Unreal.
