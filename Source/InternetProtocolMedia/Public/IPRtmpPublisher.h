/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "AudioDeviceHandle.h"
#include "IPRtmpPublishTypes.h"
#include "Tickable.h"
#include "UObject/Object.h"
#include "IPRtmpPublisher.generated.h"

namespace IPMedia {
	class FPublishPipeline;
	class FViewportCaptureHook;
	class FSubmixCapture;
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIPRtmpPublisherStateChanged, EIPRtmpPublisherState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FIPRtmpPublisherEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIPRtmpPublisherError, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FIPRtmpPublisherReconnecting, int32, Attempt);

/**
 * Publishes what the game renders (viewport, render target or scene capture) and plays (a submix) to an RTMP or
 * RTMPS server: MediaMTX, nginx-rtmp, Twitch, YouTube...
 *
 * Video: hardware H.264 through the engine's AVCodecs (NVENC, then AMF; Experimental in UE 5.8). There is no
 * software fallback: without a supported NVIDIA/AMD GPU, Start() reports an error through OnError.
 * Audio: AAC-LC 48 kHz stereo (Media Foundation).
 *
 * The stream key is only kept in memory by the network session: it is not a property, is never saved and is
 * masked in every log and error message. Capture never reads pixels back to the CPU. Everything heavy runs off the
 * Game Thread; events are broadcast on the Game Thread.
 */
UCLASS(BlueprintType, Category = "IP|RTMP")
class INTERNETPROTOCOLMEDIA_API UIPRtmpPublisher : public UObject, public FTickableGameObject {
	GENERATED_BODY()

public:
	/**
	 * Url: rtmp://host[:port]/app or rtmps://host[:port]/app. StreamKey is appended as the stream name (leave it
	 * empty if the URL already ends with it). Returns false (and OnError) on invalid input.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	bool Start(const FString& Url, const FString& StreamKey, const FIPRtmpPublishConfig& InConfig);

	/** Graceful stop. */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void Stop();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	EIPRtmpPublisherState GetState() const { return State; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	bool IsPublishing() const { return State == EIPRtmpPublisherState::Publishing; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	FIPRtmpPublisherStats GetStats() const;

	/** Audio for EIPPublishAudioSource::Manual: interleaved float samples (-1..1), any channel count and rate. */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void PushAudioSamples(const TArray<float>& Interleaved, int32 NumChannels, int32 SampleRate);

	/** Same as PushAudioSamples, for C++ (any thread). */
	void PushAudio(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate);

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpPublisherStateChanged OnStateChanged;

	/** The server accepted the stream (also after a reconnection). */
	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpPublisherEvent OnPublishing;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpPublisherReconnecting OnReconnecting;

	/** Never contains the stream key. */
	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FIPRtmpPublisherError OnError;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override { return Pipeline.IsValid(); }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return true; }

	virtual void BeginDestroy() override;

private:
	void Shutdown();
	void SetState(EIPRtmpPublisherState NewState);
	void Fail(const FString& Message);
	void CaptureFrame(int64 TimestampMs);

	UPROPERTY(Transient)
	FIPRtmpPublishConfig Config;

	/** Created when a scene capture has no target. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> OwnedTarget = nullptr;

	TSharedPtr<IPMedia::FPublishPipeline, ESPMode::ThreadSafe> Pipeline;
	TSharedPtr<IPMedia::FViewportCaptureHook, ESPMode::ThreadSafe> ViewportHook;
	TSharedPtr<IPMedia::FSubmixCapture, ESPMode::ThreadSafe> SubmixListener;
	FAudioDeviceHandle AudioDevice;
	TWeakObjectPtr<USoundSubmix> ListenedSubmix;
	EIPRtmpPublisherState State = EIPRtmpPublisherState::Idle;
	double NextCaptureTime = 0.0;
};
