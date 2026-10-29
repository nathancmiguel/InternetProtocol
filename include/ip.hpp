/**
 * MIT License (MIT)
 * Copyright © 2025 Nathan Miguel
 *
 * InternetProtocol umbrella header (UDP, TCP, HTTP, WebSocket on top of standalone ASIO).
 */

#pragma once

#include "ip/core/config.hpp"
#include "ip/core/error.hpp"
#include "ip/core/runtime.hpp"
#include "ip/core/types.hpp"
#include "ip/events/event.hpp"

#include "ip/utils/string.hpp"
#include "ip/utils/package.hpp"
#include "ip/utils/net.hpp"
#include "ip/utils/httpparser.hpp"
#include "ip/utils/dataframe.hpp"
#include "ip/utils/handshake.hpp"
#include "ip/utils/tls.hpp"

#include "ip/transport/connection.hpp"
#include "ip/transport/stream_client.hpp"
#include "ip/transport/stream_server.hpp"
#include "ip/transport/remote.hpp"

#include "ip/udp/udpclient.hpp"
#include "ip/udp/udpserver.hpp"
#include "ip/tcp/tcpclient.hpp"
#include "ip/tcp/tcpserver.hpp"
#include "ip/http/httpclient.hpp"
#include "ip/http/httpserver.hpp"
#include "ip/websocket/wsclient.hpp"
#include "ip/websocket/wsserver.hpp"
