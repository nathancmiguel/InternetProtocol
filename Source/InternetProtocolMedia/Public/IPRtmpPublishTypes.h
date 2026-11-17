/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IPRtmpPublishTypes.generated.h"

class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
class USoundSubmix;

/** What the publisher encodes as video. */
UENUM(BlueprintType)
enum class EIPPublishVideoSource : uint8 {
	/** The game viewport's back buffer (what the player sees, UI included). */
	Viewport,
	/** A render target you draw into (materials, UMG widget renderer, canvas...). */
	RenderTarget,
	/** A scene capture component (another camera); it is captured at the publish frame rate. */
	SceneCapture,
};

/** Where the published audio comes from. */
UENUM(BlueprintType)
enum class EIPPublishAudioSource : uint8 {
	/** No audio track. */
	None,
	/** The engine's audio output: the main submix, or the submix set in Submix. */
	Submix,
	/** Samples pushed from C++ with UIPRtmpPublisher::PushAudio (microphone, generated audio...). */
	Manual,
};

UENUM(BlueprintType)
enum class EIPRtmpPublisherState : uint8 {
	Idle,
	Connecting,
	Publishing,
	/** Connection lost: retrying with exponential backoff. */
	Reconnecting,
	/** Stopped because of an error (see OnError). */
	Error,
};

USTRUCT(BlueprintType)
struct INTERNETPROTOCOLMEDIA_API FIPRtmpPublishConfig {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video")
	EIPPublishVideoSource VideoSource = EIPPublishVideoSource::Viewport;

	/** Used when VideoSource is RenderTarget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget = nullptr;

	/** Used when VideoSource is SceneCapture. If it has no TextureTarget, one of the output size is created. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video")
	TObjectPtr<USceneCaptureComponent2D> SceneCapture = nullptr;

	/** Output size (the source is scaled). Even values. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video", meta = (ClampMin = "16"))
	int32 Width = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video", meta = (ClampMin = "16"))
	int32 Height = 720;

	/** Capture rate. Frames are skipped when the game runs faster; it cannot exceed the game frame rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video", meta = (ClampMin = "1", ClampMax = "120"))
	int32 FrameRate = 30;

	/** Constant bitrate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video", meta = (ClampMin = "100"))
	int32 VideoBitrateKbps = 4500;

	/** Twitch/YouTube require 2 s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Video", meta = (ClampMin = "1", ClampMax = "10"))
	int32 KeyframeIntervalSeconds = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Audio")
	EIPPublishAudioSource AudioSource = EIPPublishAudioSource::Submix;

	/** Submix to capture (empty = main submix). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Audio")
	TObjectPtr<USoundSubmix> Submix = nullptr;

	/** AAC-LC 48 kHz stereo; 96, 128, 160 or 192. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Audio")
	int32 AudioBitrateKbps = 160;

	/** Reconnect with exponential backoff (1..30 s) when the connection drops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Network")
	bool bReconnect = true;

	/** 0 = retry forever. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IP|RTMP|Network", meta = (ClampMin = "0"))
	int32 MaxReconnectAttempts = 0;
};

USTRUCT(BlueprintType)
struct INTERNETPROTOCOLMEDIA_API FIPRtmpPublisherStats {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	EIPRtmpPublisherState State = EIPRtmpPublisherState::Idle;

	/** Encoded video frames per second. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float Fps = 0.0f;

	/** Bytes actually written to the network, kbit/s. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float SentKbps = 0.0f;

	/** Video frames not encoded (encoder busy) or not sent (network congestion). */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int64 DroppedVideoFrames = 0;

	/** Audio frames lost (capture overrun or network congestion). */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int64 DroppedAudioFrames = 0;

	/** Bytes waiting in the send queue. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int64 QueuedBytes = 0;

	/** Round-trip time to the server, ms (-1 when unknown). */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float RttMs = -1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 ReconnectAttempts = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	FString VideoEncoder;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	FString AudioEncoder;
};
