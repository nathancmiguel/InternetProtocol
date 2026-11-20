/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "tcp/tcpserver.h"
#include "tcp/tcpclient.h"
#include "udp/udpserver.h"
#include "udp/udpclient.h"
#include "http/httpserver.h"
#include "http/httpclient.h"
#include "websocket/wsserver.h"
#include "websocket/wsclient.h"
#include "IPTestListener.generated.h"

/** Echoes everything a single server-side connection receives. Test-only. */
UCLASS(Transient, NotBlueprintable)
class UIPEcho : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY() TObjectPtr<UTCPRemote> Tcp;
	UPROPERTY() TObjectPtr<UTCPRemote> TcpSsl;
	UPROPERTY() TObjectPtr<UWSRemote> Ws;
	UPROPERTY() TObjectPtr<UWSRemote> Wss;

	UFUNCTION() void OnTcpBytes(const TArray<uint8>& Buffer, int Bytes) {
		if (Tcp) { Tcp->WriteBuffer(Buffer, FDelegateTcpRemoteMessageSent()); }
		if (TcpSsl) { TcpSsl->WriteBuffer(Buffer, FDelegateTcpRemoteMessageSent()); }
	}
	UFUNCTION() void OnWsMessage(const TArray<uint8>& Buffer, bool bIsBinary) {
		if (bIsBinary) {
			if (Ws) { Ws->WriteBuffer(Buffer, FDataframe(), FDelegateWsRemoteMessageSent()); }
			if (Wss) { Wss->WriteBuffer(Buffer, FDataframe(), FDelegateWsRemoteMessageSent()); }
			return;
		}
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Buffer.Num());
		const FString Message(Text.Length(), Text.Get());
		if (Ws) { Ws->Write(Message, FDataframe(), FDelegateWsRemoteMessageSent()); }
		if (Wss) { Wss->Write(Message, FDataframe(), FDelegateWsRemoteMessageSent()); }
	}
};

/** Records every event it receives; servers echo what they receive. Test-only. */
UCLASS(Transient, NotBlueprintable)
class UIPTestListener : public UObject
{
	GENERATED_BODY()
public:
	// Generic counters
	int32 Connected = 0;
	int32 Closed = 0;
	int32 Errors = 0;
	int32 Accepted = 0;
	int32 Sent = 0;
	int32 SendErrors = 0;
	FErrorCode LastError;
	TArray<uint8> Received;
	int32 Messages = 0;
	int32 LastCloseCode = 0;
	FString LastCloseReason;
	bool bLastBinary = false;
	TArray<FHttpResponse> Responses;
	TArray<FErrorCode> ResponseErrors;
	FUdpEndpoint LastUdpSender;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> Remotes;

	// ---- stream events ----
	UFUNCTION() void OnConnected() { ++Connected; }
	UFUNCTION() void OnClose() { ++Closed; }
	UFUNCTION() void OnError(const FErrorCode& Error) { ++Errors; LastError = Error; }
	UFUNCTION() void OnBytes(const TArray<uint8>& Buffer, int Bytes) { ++Messages; Received.Append(Buffer); }
	UFUNCTION() void OnSent(const FErrorCode& Error, int Bytes) { if (Error.Value == 0) { ++Sent; } else { ++SendErrors; } }

	// ---- TCP server ----
	UFUNCTION() void OnTcpAccepted(UTCPRemote* Remote) {
		++Accepted;
		UIPEcho* Echo = NewObject<UIPEcho>(this);
		Echo->Tcp = Remote;
		Remote->OnMessage.AddDynamic(Echo, &UIPEcho::OnTcpBytes);
		Remotes.Add(Echo);
	}
	UFUNCTION() void OnTcpSslAccepted(UTCPRemote* Remote) {
		++Accepted;
		UIPEcho* Echo = NewObject<UIPEcho>(this);
		Echo->TcpSsl = Remote;
		Remote->OnMessage.AddDynamic(Echo, &UIPEcho::OnTcpBytes);
		Remotes.Add(Echo);
	}

	// ---- UDP ----
	UPROPERTY() TObjectPtr<UUDPServer> UdpServer;
	UFUNCTION() void OnUdpServerMessage(const TArray<uint8>& Buffer, int Bytes, const FUdpEndpoint& Endpoint) {
		++Messages;
		LastUdpSender = Endpoint;
		if (UdpServer) {
			// Echo using only Address/Port, as a Blueprint would.
			FUdpEndpoint Target;
			Target.Address = Endpoint.Address;
			Target.Port = Endpoint.Port;
			UdpServer->SendBufferTo(Buffer, Target, FDelegateUdpServerMessageSent());
		}
	}

	// ---- HTTP ----
	UFUNCTION() void OnHttpResponse(const FErrorCode& Error, const FHttpResponse& Response) {
		ResponseErrors.Add(Error);
		Responses.Add(Response);
	}
	UFUNCTION() void OnHttpHello(const FHttpRequest& Request, UHttpRemote* Remote) {
		const FString* Name = Request.Params.Find(TEXT("name"));
		Remote->Headers.Body = FString::Printf(TEXT("hello %s"), Name ? **Name : TEXT("?"));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpEcho(const FHttpRequest& Request, UHttpRemote* Remote) {
		Remote->Headers.Status_Code = 201;
		Remote->Headers.Body = Request.Body;
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}

	// ---- raw TCP peers for HTTP edge cases ----
	int32 RemoteClosed = 0;
	FString RawReply;
	UPROPERTY() TArray<TObjectPtr<UTCPRemote>> RawRemotes;
	/** Answers every request with RawReply verbatim (crafted HTTP responses); empty RawReply = never answers. */
	UFUNCTION() void OnRawAccepted(UTCPRemote* Remote) {
		++Accepted;
		RawRemotes.Add(Remote);
		Remote->OnMessage.AddDynamic(this, &UIPTestListener::OnRawRequest);
		Remote->OnClose.AddDynamic(this, &UIPTestListener::OnRemoteClose);
	}
	UFUNCTION() void OnRawRequest(const TArray<uint8>& Buffer, int Bytes) {
		++Messages;
		if (!RawReply.IsEmpty()) {
			for (UTCPRemote* Remote : RawRemotes) {
				if (IsValid(Remote)) {
					Remote->Write(RawReply, FDelegateTcpRemoteMessageSent());
				}
			}
		}
	}
	UFUNCTION() void OnRemoteClose() { ++RemoteClosed; }
	/** HTTP route that never answers (the server must send 503 by itself). */
	UFUNCTION() void OnHttpMute(const FHttpRequest& Request, UHttpRemote* Remote) { ++Messages; }

	// ---- HTTP routing ----
	UFUNCTION() void OnHttpUser(const FHttpRequest& Request, UHttpRemote* Remote) {
		const FString* Id = Request.RouteParams.Find(TEXT("id"));
		Remote->Headers.Body = FString::Printf(TEXT("user %s"), Id ? **Id : TEXT("?"));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpFiles(const FHttpRequest& Request, UHttpRemote* Remote) {
		const FString* Rest = Request.RouteParams.Find(TEXT("*"));
		Remote->Headers.Body = Rest ? *Rest : FString(TEXT("?"));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpCookies(const FHttpRequest& Request, UHttpRemote* Remote) {
		for (const TCHAR* Cookie : {TEXT("a=1; Path=/"), TEXT("b=2, c; Path=/")}) {
			FHttpHeaderField& Header = Remote->Headers.HeaderList.AddDefaulted_GetRef();
			Header.Name = TEXT("Set-Cookie");
			Header.Value = Cookie;
		}
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}

	// ---- Phase 3 ----
	int32 Progress = 0;
	UFUNCTION() void OnProgressEvent(int32 RequestId, int64 BytesSoFar, int64 Total) { ++Progress; }
	UFUNCTION() void OnHttpEchoBytes(const FHttpRequest& Request, UHttpRemote* Remote) {
		Remote->Headers.BodyBytes = Request.BodyBytes;
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpRedirect(const FHttpRequest& Request, UHttpRemote* Remote) {
		Remote->Headers.Status_Code = 302;
		Remote->Headers.Headers.Add(TEXT("Location"), TEXT("/new?from=old"));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpNew(const FHttpRequest& Request, UHttpRemote* Remote) {
		const FString* From = Request.Params.Find(TEXT("from"));
		Remote->Headers.Body = FString::Printf(TEXT("new from %s"), From ? **From : TEXT("?"));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpBig(const FHttpRequest& Request, UHttpRemote* Remote) {
		Remote->Headers.Body = FString::ChrN(1024 * 1024, TEXT('z'));
		Remote->Write(FDelegateHttpRemoteMessageSent());
	}
	UFUNCTION() void OnHttpStream(const FHttpRequest& Request, UHttpRemote* Remote) {
		Remote->BeginStream();
		Remote->WriteChunk(TEXT("a"));
		Remote->WriteChunk(TEXT("bc"));
		Remote->WriteChunk(TEXT("def"));
		Remote->EndStream();
	}

	// ---- WebSocket extras ----
	int32 Pongs = 0;
	int32 Upgrades = 0;
	UFUNCTION() void OnPongEvent() { ++Pongs; }
	/** Accepts upgrade requests carrying "X-Token: secret", rejects the others with 401. */
	UFUNCTION() void OnWsUpgrade(UWSRemote* Remote, const FHttpRequest& Request) {
		++Upgrades;
		const FString* Token = Request.Headers.Find(TEXT("x-token"));
		if (Token && *Token == TEXT("secret")) {
			Remote->AcceptUpgrade();
		} else {
			Remote->RejectUpgrade(401, TEXT("bad token"));
		}
	}

	// ---- WebSocket ----
	UFUNCTION() void OnWsConnected(const FHttpResponse& Handshake) { ++Connected; }
	UFUNCTION() void OnWsUnexpected(const FHttpResponse& Handshake) { ++Errors; }
	UFUNCTION() void OnWsMessage(const TArray<uint8>& Buffer, bool bIsBinary) { ++Messages; bLastBinary = bIsBinary; Received = Buffer; }
	UFUNCTION() void OnWsClose(int Code, const FString& Reason) { ++Closed; LastCloseCode = Code; LastCloseReason = Reason; }
	UFUNCTION() void OnWsAccepted(UWSRemote* Remote) {
		++Accepted;
		UIPEcho* Echo = NewObject<UIPEcho>(this);
		Echo->Ws = Remote;
		Remote->OnMessage.AddDynamic(Echo, &UIPEcho::OnWsMessage);
		Remotes.Add(Echo);
	}
	UFUNCTION() void OnWssAccepted(UWSRemote* Remote) {
		++Accepted;
		UIPEcho* Echo = NewObject<UIPEcho>(this);
		Echo->Wss = Remote;
		Remote->OnMessage.AddDynamic(Echo, &UIPEcho::OnWsMessage);
		Remotes.Add(Echo);
	}
};
