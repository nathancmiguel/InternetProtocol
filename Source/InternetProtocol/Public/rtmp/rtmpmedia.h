/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "net/asio.h"

/*
 * Native (C++) access to media published to an RTMP ingest server of this plugin.
 *
 * Threading contract:
 *  - IRtmpMediaSink methods are called on the network strand of the publishing connection. They must
 *    return quickly and must not block (copy/enqueue only). They must not touch UObjects.
 *  - Calls for one stream are serialized; different streams may call concurrently.
 *  - FRtmpIngestHub methods are thread-safe.
 *
 * A sink registered while a stream is already live receives OnStreamStarted, then the stored sequence
 * headers (avcC / AudioSpecificConfig), and video starts at the next keyframe.
 */

struct FRtmpStreamInfo {
	FString App;
	FString StreamName;
	uint16 Port = 0;
	FTcpEndpoint Remote;
	/** From onMetaData when the publisher sends it (0 when unknown). */
	double Width = 0.0;
	double Height = 0.0;
	double FrameRate = 0.0;
	double VideoDataRateKbps = 0.0;
	double AudioSampleRate = 0.0;
	double AudioChannels = 0.0;
	FString Encoder;
};

enum class ERtmpPacketType : uint8 {
	Video,
	Audio,
};

struct FRtmpMediaPacket {
	ERtmpPacketType Type = ERtmpPacketType::Video;
	/** Decode timestamp in ms, made monotonic across the 32-bit RTMP wrap. */
	int64 DtsMs = 0;
	/** Video: composition time offset (PTS = DTS + Cts). Audio: 0. */
	int32 CtsMs = 0;
	bool bKeyframe = false;
	/** Video: payload is an avcC record. Audio: payload is an AudioSpecificConfig. */
	bool bSequenceHeader = false;
	/** FLV codec id (7 = H.264) or sound format (10 = AAC). */
	uint8 Codec = 0;
	/** Video: AVCC NAL units (or avcC). Audio: raw AAC frame (or ASC). Shared, never modified. */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Payload;
};

class IRtmpMediaSink {
public:
	virtual ~IRtmpMediaSink() = default;
	virtual void OnStreamStarted(const FRtmpStreamInfo& Info) = 0;
	virtual void OnMediaPacket(const FRtmpMediaPacket& Packet) = 0;
	/** The publisher stopped or disconnected. More streams may start later for the same key. */
	virtual void OnStreamEnded() = 0;
};

class INTERNETPROTOCOL_API FRtmpIngestHub {
public:
	/**
	 * Delivers the media of (Port, App, StreamName) to Sink, now and for every future publish of that key,
	 * until UnregisterSink. Returns a handle (> 0).
	 */
	static int64 RegisterSink(uint16 Port, const FString& App, const FString& StreamName, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink);
	static void UnregisterSink(int64 Handle);

	/**
	 * Makes sure something listens for RTMP on Port: reuses a URTMPServer opened on that port or opens an
	 * internal listener (0.0.0.0) shared by all users of the port. Returns a handle (> 0) or 0 on failure.
	 */
	static int64 AcquirePort(uint16 Port, FString& OutError);
	static void ReleasePort(int64 Handle);
};
