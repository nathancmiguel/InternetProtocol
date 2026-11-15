/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/common.h"

/*
 * Native RTMP/RTMPS publishing client (used by InternetProtocolMedia's UIPRtmpPublisher, usable from C++).
 *
 * Threading: every method is thread-safe and non-blocking. Callbacks run on a network thread (forward them
 * to the Game Thread yourself). Media must already be FLV-ready: H.264 as AVCC (4-byte lengths) plus an
 * avcC record, AAC as raw frames plus an AudioSpecificConfig.
 *
 * Congestion: the send queue is bounded. When it fills up, non-keyframe video is dropped until the next
 * keyframe; audio is only dropped when the queue is completely full. Reconnection uses exponential backoff
 * (1 s .. 30 s with jitter); sequence headers and metadata are resent and video resumes at a keyframe.
 * The stream key is never logged.
 */

enum class ERtmpPublishState : uint8 {
	Idle,
	Connecting,
	Publishing,
	Reconnecting,
	Stopped,
	Failed,
};

struct FRtmpPublishOptions {
	bool bReconnect = true;
	/** 0 = retry forever. */
	int32 MaxReconnectAttempts = 0;
	/** Upper bound of bytes waiting to be sent. */
	int64 MaxQueuedBytes = 8 * 1024 * 1024;
	/** Used for rtmps:// (verification on by default). */
	FSecurityContextOpts Tls;
	/** onMetaData values (0 = omitted). */
	double Width = 0;
	double Height = 0;
	double FrameRate = 0;
	double VideoBitrateKbps = 0;
	double AudioSampleRate = 0;
	double AudioChannels = 0;
	double AudioBitrateKbps = 0;
};

struct FRtmpPublishStats {
	int64 BytesSent = 0;
	int64 QueuedBytes = 0;
	int32 VideoFramesSent = 0;
	int32 AudioFramesSent = 0;
	int32 DroppedVideoFrames = 0;
	int32 DroppedAudioFrames = 0;
	/** Round-trip time in ms (-1 when unknown). */
	double RttMs = -1.0;
	int32 ReconnectAttempts = 0;
};

struct FRtmpPublishCallbacks {
	TFunction<void(ERtmpPublishState)> OnStateChanged;
	/** Message never contains the stream key. */
	TFunction<void(const FString& /*Message*/)> OnError;
	TFunction<void(int32 /*Attempt*/)> OnReconnecting;
};

class INTERNETPROTOCOL_API IRtmpPublishSession {
public:
	virtual ~IRtmpPublishSession() = default;

	/**
	 * Url: rtmp://host[:port]/app[/...] or rtmps://... . If StreamKey is empty the last path segment of the
	 * URL is used as the stream name. Returns nullptr (and OutError) if the URL is invalid or the network
	 * service is not running.
	 */
	static TSharedPtr<IRtmpPublishSession, ESPMode::ThreadSafe> Create(const FString& Url, const FString& StreamKey,
		const FRtmpPublishOptions& Options, FRtmpPublishCallbacks Callbacks, FString& OutError);

	/** Returns the URL with the stream key masked, safe to log or display. */
	static FString MaskUrl(const FString& Url);

	virtual void Start() = 0;
	/** Graceful stop (FCUnpublish/deleteStream); no reconnection afterwards. */
	virtual void Stop() = 0;

	virtual void SetVideoConfig(TArray<uint8> AvcDecoderConfig) = 0;
	virtual void SetAudioConfig(TArray<uint8> AudioSpecificConfig) = 0;
	/** Returns false if the frame was dropped (not publishing, congestion). */
	virtual bool SendVideo(int64 DtsMs, int32 CtsMs, bool bKeyframe, TArray<uint8> Avcc) = 0;
	virtual bool SendAudio(int64 DtsMs, TArray<uint8> RawAac) = 0;

	virtual ERtmpPublishState GetState() const = 0;
	virtual FRtmpPublishStats GetStats() const = 0;
};
