/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/asio.h"
#include <memory>

/*
 * Forward declarations of the native (non-UObject) networking layer and the Game Thread state cached by the
 * UObject facades. Public UObject headers only hold std::shared_ptr to native types; their definitions live
 * in Private/net.
 *
 * Ownership model (see Private/net/ioservice.h for the full picture):
 *  - UObjects live on the Game Thread and own a std::shared_ptr to their native session.
 *  - Native sessions live on an Asio strand. Pending handlers keep the session alive, never the UObject.
 *  - Results travel back to the Game Thread through AsyncTask with a TWeakObjectPtr to the UObject and the
 *    id of the session that produced them, so events of a previous connection are never mixed with the
 *    current one.
 *  - Every member below is read and written on the Game Thread only.
 */
namespace IPNet {
	class FTransport;
	class FUdpSocket;
	class FTcpAcceptor;
	struct FTlsConfig;
	class FHttpParser;
	class FHttpServerSession;
	class FWsSession;
	struct FFacadeAccess;
	struct FHttpAccess;
	struct FWsAccess;
}

/** Game Thread state of a stream (TCP/TLS) connection facade. */
struct FIpStreamState {
	std::shared_ptr<IPNet::FTransport> Transport;
	/** Session whose events are currently delivered (0 = none). */
	uint64 SessionId = 0;
	/** Session closed by the user whose OnClose has not been delivered yet. */
	uint64 PendingCloseId = 0;
	bool bOpen = false;
	FTcpEndpoint Local;
	FTcpEndpoint Remote;
	FErrorCode LastError;
};

/** Game Thread state of a listening server facade. */
struct FIpServerState {
	std::shared_ptr<IPNet::FTcpAcceptor> Acceptor;
	uint64 SessionId = 0;
	uint64 PendingCloseId = 0;
	bool bOpen = false;
	FTcpEndpoint Local;
	FErrorCode LastError;
};

/** Game Thread state of a UDP facade. */
struct FIpUdpState {
	std::shared_ptr<IPNet::FUdpSocket> Socket;
	uint64 SessionId = 0;
	uint64 PendingCloseId = 0;
	bool bOpen = false;
	FUdpEndpoint Local;
	FUdpEndpoint Remote;
	FErrorCode LastError;
};

/** Game Thread state of a WebSocket facade (client or server-side remote). */
struct FIpWsState {
	FIpStreamState Net;
	std::shared_ptr<IPNet::FWsSession> Session;
	/** End() was called: no more data frames are accepted. */
	bool bClosing = false;
};
