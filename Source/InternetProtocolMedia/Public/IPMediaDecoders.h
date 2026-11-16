/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IMediaTextureSample.h"
#include "IMediaAudioSample.h"
#include "Templates/Function.h"

/*
 * Pluggable decoders for the RTMP receiver (C++ only).
 * The default implementation uses the engine's Electra decoders (Media Foundation, hardware accelerated
 * through D3D11/D3D12 when available). Register a factory to replace them, e.g. with a custom software
 * decoder:
 *
 *   FIPMediaDecoders::SetVideoDecoderFactory([]() { return MakeShared<FMyDecoder, ESPMode::ThreadSafe>(); });
 *
 * Threading: a decoder instance is created, used and destroyed on one decode thread. It must never touch
 * UObjects. Output samples are handed to the Media Framework, which renders them on the render thread.
 */

/** One access unit in player time (already re-based by the receiver). */
struct FIPAccessUnit {
	const uint8* Data = nullptr;
	int32 Size = 0;
	FTimespan Dts;
	FTimespan Pts;
	FTimespan Duration;
	bool bKeyframe = false;
};

enum class EIPDecodeResult : uint8 {
	/** Access unit consumed. */
	Ok,
	/** Decoder input is full: fetch outputs, then retry the same access unit. */
	Busy,
	/** Unrecoverable for this decoder instance; GetLastError() explains. */
	Error,
};

class IIPVideoDecoder {
public:
	virtual ~IIPVideoDecoder() = default;
	virtual FString GetName() const = 0;
	/** AvcDecoderConfig: avcC record (SPS/PPS). Called again with a new record when the stream changes. */
	virtual bool Configure(const TArray<uint8>& AvcDecoderConfig, int32 MaxWidth, int32 MaxHeight) = 0;
	/** Data is H.264 in AVCC form (4-byte NAL lengths). */
	virtual EIPDecodeResult Decode(const FIPAccessUnit& AccessUnit) = 0;
	/** Returns the next decoded frame in presentation order, if any. */
	virtual bool GetOutput(TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe>& OutSample, FIntPoint& OutDimensions) = 0;
	/** Outputs pending frames (end of stream / before reconfiguration). */
	virtual void Drain() = 0;
	virtual void Close() = 0;
	virtual FString GetLastError() const = 0;
};

class IIPAudioDecoder {
public:
	virtual ~IIPAudioDecoder() = default;
	virtual FString GetName() const = 0;
	/** AudioSpecificConfig of the AAC stream. */
	virtual bool Configure(const TArray<uint8>& AudioSpecificConfig, int32 SampleRate, int32 NumChannels) = 0;
	/** Data is one raw AAC frame. */
	virtual EIPDecodeResult Decode(const FIPAccessUnit& AccessUnit) = 0;
	/** Returns the next decoded block (interleaved float PCM sample). */
	virtual bool GetOutput(TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe>& OutSample) = 0;
	virtual void Close() = 0;
	virtual FString GetLastError() const = 0;
};

using FIPVideoDecoderFactory = TFunction<TSharedPtr<IIPVideoDecoder, ESPMode::ThreadSafe>()>;
using FIPAudioDecoderFactory = TFunction<TSharedPtr<IIPAudioDecoder, ESPMode::ThreadSafe>()>;

class INTERNETPROTOCOLMEDIA_API FIPMediaDecoders {
public:
	/** Replaces the decoder used by receivers opened afterwards. Pass an empty function to restore Electra. */
	static void SetVideoDecoderFactory(FIPVideoDecoderFactory Factory);
	static void SetAudioDecoderFactory(FIPAudioDecoderFactory Factory);

	/** Creates a decoder with the registered factory (Electra by default). Thread-safe. */
	static TSharedPtr<IIPVideoDecoder, ESPMode::ThreadSafe> CreateVideoDecoder();
	static TSharedPtr<IIPAudioDecoder, ESPMode::ThreadSafe> CreateAudioDecoder();
};
