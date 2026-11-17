/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "UObject/Object.h"
#include "IPMediaTypes.h"
#include "IPRtmpReceiver.generated.h"

class AActor;
class UMediaPlayer;
class UMediaSoundComponent;
class UMediaTexture;
class URTMPServer;
class UIPRtmpMediaSource;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FIPRtmpReceiverEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIPRtmpReceiverError, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIPRtmpReceiverFrame, const FIPVideoFrameInfo&, Frame);

/**
 * Receives an RTMP stream published to this machine (OBS, FFmpeg, another Unreal instance) and exposes it
 * as a UMediaTexture (materials / UMG) and a UMediaSoundComponent (3D audio with attenuation).
 *
 * Built on the Media Framework through the iprtmp:// player: video is decoded by the engine's Electra
 * decoders (hardware accelerated) and stays on the GPU; A/V sync uses the audio as master clock.
 * All functions and events are Game Thread only. OnFrame carries metadata only, never pixels, and fires at
 * most once per engine tick.
 */
UCLASS(BlueprintType, Category = "IP|RTMP")
class INTERNETPROTOCOLMEDIA_API UIPRtmpReceiver : public UObject, public FTickableGameObject
{
	GENERATED_BODY()
public:
	/**
	 * Starts receiving:
	 *  - iprtmp://host:port/app/key: a stream published to this machine. The port is served by a URTMPServer
	 *    opened on it or by an internal listener.
	 *  - rtmp://host[:port]/app/key or rtmps://...: plays the stream from that RTMP server (MediaMTX, nginx-rtmp,
	 *    a URTMPServer with playback enabled...). Reconnects with backoff if the connection drops.
	 * LatencyMs = 0 uses the default of the mode (150 / 1000 ms).
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	bool StartUrl(const FString& Url, EIPRtmpLatencyMode LatencyMode = EIPRtmpLatencyMode::Smooth, int32 LatencyMs = 0);

	/**
	 * Plays a stream from an RTMP server: Url = rtmp(s)://host[:port]/app, StreamKey kept in memory only (not part of
	 * the URL, never logged). Server refusals (unknown key, TLS errors...) are reported through OnError.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	bool StartPull(const FString& Url, const FString& StreamKey, EIPRtmpLatencyMode LatencyMode = EIPRtmpLatencyMode::Smooth, int32 LatencyMs = 0);

	/** Receives the stream published to Server with the given app and stream key. */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	bool StartFromServer(URTMPServer* Server, const FString& App, const FString& StreamKey,
	                     EIPRtmpLatencyMode LatencyMode = EIPRtmpLatencyMode::Smooth, int32 LatencyMs = 0);

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void Stop();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	bool IsActive() const { return bActive; }

	/** True while a publisher is sending the stream. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	bool IsStreaming() const { return bStreaming; }

	/** Texture usable in materials and UMG (valid after Start). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	UMediaTexture* GetVideoTexture() const { return VideoTexture; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	UMediaPlayer* GetMediaPlayer() const { return MediaPlayer; }

	/**
	 * Creates (or returns) a spatialized sound component playing the stream audio, attached to Owner's root.
	 * Configure attenuation on the returned component as for any audio component.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	UMediaSoundComponent* CreateAudioComponent(AActor* Owner);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	FIPRtmpReceiverStats GetStats() const;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpReceiverEvent OnStarted;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpReceiverEvent OnEnded;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpReceiverError OnError;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpReceiverFrame OnFrame;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override { return bActive; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return true; }

	virtual void BeginDestroy() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UMediaPlayer> MediaPlayer;

	UPROPERTY(Transient)
	TObjectPtr<UMediaTexture> VideoTexture;

	UPROPERTY(Transient)
	TObjectPtr<UIPRtmpMediaSource> Source;

	UPROPERTY(Transient)
	TObjectPtr<UMediaSoundComponent> SoundComponent;

	bool bActive = false;
	bool bStreaming = false;
	uint64 LastFrameCounter = 0;
};
