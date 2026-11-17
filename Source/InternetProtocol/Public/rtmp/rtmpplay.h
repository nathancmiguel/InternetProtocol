/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/common.h"
#include "rtmp/rtmpmedia.h"

/*
 * Native RTMP/RTMPS playback client ("pull"): connects to an RTMP server (MediaMTX, nginx-rtmp, another Unreal
 * URTMPServer with playback enabled, ...), sends "play" and hands the received media to an IRtmpMediaSink
 * (InternetProtocolMedia uses it for rtmp:// URLs in UIPRtmpReceiver).
 *
 * Threading: every method is thread-safe and non-blocking. The sink and the callbacks are called on a network
 * strand: copy/enqueue only, never block, never touch UObjects.
 *
 * The sink sees OnStreamStarted (with onMetaData values when the server sends them), the sequence headers,
 * media, and OnStreamEnded when the server reports the stream stopped or the connection drops. After a
 * reconnection (exponential backoff 1 s .. 30 s with jitter) a new OnStreamStarted follows. TLS certificates are
 * always verified (configure FRtmpPlayOptions::Tls). The stream key is never logged.
 */

enum class ERtmpPlayState : uint8 {
	Idle,
	Connecting,
	Playing,
	Reconnecting,
	Stopped,
	Failed,
};

struct FRtmpPlayOptions {
	bool bReconnect = true;
	/** 0 = retry forever. */
	int32 MaxReconnectAttempts = 0;
	/** Client buffer hint sent to the server (SetBufferLength), ms. */
	int32 BufferMs = 500;
	/** Used for rtmps:// (verification on by default). */
	FSecurityContextOpts Tls;
};

struct FRtmpPlayStats {
	int64 BytesReceived = 0;
	int32 VideoFramesReceived = 0;
	int32 AudioFramesReceived = 0;
	/** Round-trip time in ms (-1 when unknown). */
	double RttMs = -1.0;
	int32 ReconnectAttempts = 0;
};

struct FRtmpPlayCallbacks {
	TFunction<void(ERtmpPlayState)> OnStateChanged;
	/** Message never contains the stream key. */
	TFunction<void(const FString& /*Message*/)> OnError;
	TFunction<void(int32 /*Attempt*/)> OnReconnecting;
};

class INTERNETPROTOCOL_API IRtmpPlaySession {
public:
	virtual ~IRtmpPlaySession() = default;

	/**
	 * Url: rtmp://host[:port]/app[/key] or rtmps://... . If StreamKey is empty the last path segment of the URL is
	 * used. Returns nullptr (and OutError) if the URL is invalid or the network service is not running.
	 */
	static TSharedPtr<IRtmpPlaySession, ESPMode::ThreadSafe> Create(const FString& Url, const FString& StreamKey, const FRtmpPlayOptions& Options,
		TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink, FRtmpPlayCallbacks Callbacks, FString& OutError);

	virtual void Start() = 0;
	/** Closes the connection; no reconnection afterwards. The sink gets OnStreamEnded if a stream was playing. */
	virtual void Stop() = 0;
	virtual ERtmpPlayState GetState() const = 0;
	virtual FRtmpPlayStats GetStats() const = 0;
};
