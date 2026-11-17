/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "BaseMediaSource.h"
#include "IPMediaTypes.h"
#include "IPRtmpMediaSource.generated.h"

/**
 * Media source for an RTMP stream. Works with UMediaPlayer, UMediaTexture, UMediaSoundComponent and Media Plate.
 *
 * Url:
 *  - iprtmp://0.0.0.0:1935/<app>/<stream key>: a stream published to this machine (OBS, FFmpeg, another Unreal).
 *    The port is served by a URTMPServer opened on it or, when none exists, by an internal listener.
 *  - rtmp://host[:port]/<app>[/<key>] or rtmps://...: plays the stream from that RTMP server (pull).
 *
 * Keys in the URL end up in saved assets: prefer setting the URL at runtime, or SetStreamKey (memory only).
 */
UCLASS(BlueprintType, hidecategories = (Overrides, Playback))
class INTERNETPROTOCOLMEDIA_API UIPRtmpMediaSource : public UBaseMediaSource
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP", meta = (DisplayName = "URL"))
	FString StreamUrl = TEXT("iprtmp://0.0.0.0:1935/live/stream");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP")
	EIPRtmpLatencyMode LatencyMode = EIPRtmpLatencyMode::Smooth;

	/** Jitter buffer target in ms (0 = default of the mode: 150 / 1000). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP", meta = (ClampMin = "0", ClampMax = "10000"))
	int32 LatencyMs = 0;

	static const FName LatencyModeOption;
	static const FName LatencyMsOption;
	static const FName StreamKeyOption;

	/** Stream key for rtmp(s):// URLs, kept in memory only (never saved). Empty = last path segment of the URL. */
	UFUNCTION(BlueprintCallable, Category = "RTMP")
	void SetStreamKey(const FString& InStreamKey) { StreamKey = InStreamKey; }

	virtual FString GetUrl() const override { return StreamUrl; }
	virtual bool Validate() const override;

	using UBaseMediaSource::GetMediaOption;
	virtual bool GetMediaOption(const FName& Key, bool DefaultValue) const override;
	virtual int64 GetMediaOption(const FName& Key, int64 DefaultValue) const override;
	virtual FString GetMediaOption(const FName& Key, const FString& DefaultValue) const override;
	virtual double GetMediaOption(const FName& Key, double DefaultValue) const override;
	virtual FText GetMediaOption(const FName& Key, const FText& DefaultValue) const override;
	virtual bool HasMediaOption(const FName& Key) const override;

private:
	FString StreamKey;
};
