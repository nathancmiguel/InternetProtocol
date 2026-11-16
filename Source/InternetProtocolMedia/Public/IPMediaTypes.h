/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IPMediaTypes.generated.h"

/** Receiver latency policy. */
UENUM(BlueprintType)
enum class EIPRtmpLatencyMode : uint8 {
	/** Small jitter buffer (default 150 ms): lowest delay, more rebuffering on bad networks. */
	LowLatency,
	/** Larger jitter buffer (default 1000 ms): tolerant to network jitter. */
	Smooth,
};

/** Metadata of a presented video frame (never the pixels). */
USTRUCT(BlueprintType)
struct INTERNETPROTOCOLMEDIA_API FIPVideoFrameInfo {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Width = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Height = 0;

	/** Presentation time on the player timeline, in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	double Time = 0.0;

	/** Frames per second measured over the last second. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float Fps = 0.0f;
};

USTRUCT(BlueprintType)
struct INTERNETPROTOCOLMEDIA_API FIPRtmpReceiverStats {
	GENERATED_BODY()

	/** A publisher is currently sending this stream. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	bool bStreaming = false;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Width = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Height = 0;

	/** Video frames handed to the Media Framework per second. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float Fps = 0.0f;

	/** Incoming media bitrate (audio + video), kbit/s. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float BitrateKbps = 0.0f;

	/** Buffered media ahead of presentation (jitter buffer), ms. Network/encoder delay is not included. */
	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	float BufferedMs = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 AudioSampleRate = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 AudioChannels = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int64 DroppedVideoFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int64 DroppedAudioFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Rebuffers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	int32 Discontinuities = 0;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	FString VideoDecoder;

	UPROPERTY(BlueprintReadOnly, Category = "IP|RTMP")
	FString AudioDecoder;
};
