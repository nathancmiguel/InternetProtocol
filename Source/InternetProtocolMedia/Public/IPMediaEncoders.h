/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"

/*
 * Encoders used by UIPRtmpPublisher. The defaults are the engine's AVCodecs hardware H.264 encoder (NVENC, then
 * AMF) and the Windows Media Foundation AAC encoder. Replace them from C++ (before publishing starts):
 *
 *   FIPMediaEncoders::SetVideoEncoderFactory([]() { return MakeShared<FMyEncoder, ESPMode::ThreadSafe>(); });
 *
 * Threading: an encoder instance is opened, used and closed on one encode thread. It must never touch UObjects.
 * Video input textures are complete on the GPU when Encode() is called (the publisher waits for a GPU fence).
 */

struct FIPVideoEncoderSettings {
	int32 Width = 1280;
	int32 Height = 720;
	int32 FrameRate = 30;
	int32 BitrateKbps = 4500;
	/** Distance between keyframes (streaming services expect 2 s). */
	int32 KeyframeIntervalSeconds = 2;
};

/** One encoded picture: H.264 Annex-B (start codes), no B-frames (decode order == presentation order). */
struct FIPEncodedVideoFrame {
	TArray<uint8> AnnexB;
	int64 TimestampMs = 0;
	bool bKeyframe = false;
};

class IIPVideoEncoder {
public:
	virtual ~IIPVideoEncoder() = default;
	virtual FString GetName() const = 0;
	virtual FString GetLastError() const = 0;
	virtual bool Open(const FIPVideoEncoderSettings& Settings) = 0;
	/** Texture: PF_B8G8R8A8, the size given to Open(), created with ETextureCreateFlags::Shared. */
	virtual bool Encode(const FTextureRHIRef& Texture, int64 TimestampMs, bool bForceKeyframe) = 0;
	/** Returns encoded pictures in output order, one per call. */
	virtual bool Receive(FIPEncodedVideoFrame& Out) = 0;
	virtual void Close() = 0;
};

struct FIPAudioEncoderSettings {
	/** 44100 or 48000. */
	int32 SampleRate = 48000;
	/** 1 or 2. */
	int32 NumChannels = 2;
	/** 96, 128, 160 or 192 (other values are rounded to the nearest of these). */
	int32 BitrateKbps = 160;
};

/** One raw AAC access unit (1024 samples per channel). */
struct FIPEncodedAudioFrame {
	TArray<uint8> Data;
	int64 TimestampMs = 0;
};

class IIPAudioEncoder {
public:
	virtual ~IIPAudioEncoder() = default;
	virtual FString GetName() const = 0;
	virtual FString GetLastError() const = 0;
	virtual bool Open(const FIPAudioEncoderSettings& Settings) = 0;
	/** AudioSpecificConfig describing the output (valid after Open()). */
	virtual TArray<uint8> GetAudioSpecificConfig() const = 0;
	/** Interleaved 16-bit PCM in the opened format. TimestampMs is the time of the first sample. */
	virtual bool Encode(const int16* Interleaved, int32 NumFrames, int64 TimestampMs) = 0;
	virtual bool Receive(FIPEncodedAudioFrame& Out) = 0;
	virtual void Close() = 0;
};

using FIPVideoEncoderFactory = TFunction<TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe>()>;
using FIPAudioEncoderFactory = TFunction<TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe>()>;

class INTERNETPROTOCOLMEDIA_API FIPMediaEncoders {
public:
	/** Replaces the encoder used by publishers started afterwards. Pass an empty function to restore the default. */
	static void SetVideoEncoderFactory(FIPVideoEncoderFactory Factory);
	static void SetAudioEncoderFactory(FIPAudioEncoderFactory Factory);
	/** Creates an encoder with the registered factory. Thread-safe. */
	static TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> CreateVideoEncoder();
	static TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> CreateAudioEncoder();
};
