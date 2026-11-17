/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IPMediaTypes.h"
#include "IPRtmpPublishTypes.h"
#include <atomic>
#include "IPRtmpExampleActor.generated.h"

class UIPRtmpExampleWidget;
class UIPRtmpPublisher;
class UIPRtmpReceiver;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPointLightComponent;
class USceneCaptureComponent2D;
class UStaticMeshComponent;
class UAudioComponent;
class USoundWaveProcedural;
class URTMPServer;

/**
 * RTMP demo: a screen that plays what an RTMP publisher (OBS, ffmpeg, or this actor) sends to the local server,
 * with spatialized sound, plus a publisher that streams the viewport or this actor's camera to any RTMP server.
 * Every second a light flashes and a beep plays at the same time: watch the published or received stream to judge
 * audio/video sync. Press Tab to switch between the UI and moving around.
 */
UCLASS(Blueprintable, Category = "IP|RTMP|Example")
class INTERNETPROTOCOLEXAMPLE_API AIPRtmpExampleActor : public AActor {
	GENERATED_BODY()

public:
	AIPRtmpExampleActor();

	/** Material with a texture parameter (ScreenTextureParameter) shown on the screen; the video texture is bound to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP Example")
	TObjectPtr<UMaterialInterface> ScreenMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP Example")
	FName ScreenTextureParameter = TEXT("Video");

	/** Shows the control panel on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP Example")
	bool bShowPanel = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RTMP Example")
	TObjectPtr<UStaticMeshComponent> Screen;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RTMP Example")
	TObjectPtr<USceneCaptureComponent2D> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RTMP Example")
	TObjectPtr<UStaticMeshComponent> Spinner;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RTMP Example")
	TObjectPtr<UPointLightComponent> SyncLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RTMP Example")
	TObjectPtr<UAudioComponent> SyncBeep;

	/** Opens an RTMP server on Port and plays App/StreamKey on the screen. */
	UFUNCTION(BlueprintCallable, Category = "RTMP Example")
	bool StartReceiving(int32 Port, const FString& App, const FString& StreamKey, EIPRtmpLatencyMode Mode, int32 LatencyMs);

	UFUNCTION(BlueprintCallable, Category = "RTMP Example")
	void StopReceiving();

	/** Url: rtmp(s)://host[:port]/app. */
	UFUNCTION(BlueprintCallable, Category = "RTMP Example")
	bool StartPublishing(const FString& Url, const FString& StreamKey, EIPPublishVideoSource Source, int32 Width, int32 Height, int32 FrameRate,
	                     int32 BitrateKbps, bool bAudio);

	UFUNCTION(BlueprintCallable, Category = "RTMP Example")
	void StopPublishing();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "RTMP Example")
	bool IsReceiving() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "RTMP Example")
	FIPRtmpReceiverStats GetReceiverStats() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "RTMP Example")
	FIPRtmpPublisherStats GetPublisherStats() const;

	/** Last status line for the UI (errors and events, never the stream key). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "RTMP Example")
	FString GetReceiverStatus() const { return ReceiverStatus; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "RTMP Example")
	FString GetPublisherStatus() const { return PublisherStatus; }

	UFUNCTION(BlueprintCallable, Category = "RTMP Example")
	void TogglePanelFocus();

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UFUNCTION()
	void HandleReceiverStarted();
	UFUNCTION()
	void HandleReceiverEnded();
	UFUNCTION()
	void HandleReceiverError(const FString& Message);
	UFUNCTION()
	void HandlePublisherState(EIPRtmpPublisherState State);
	UFUNCTION()
	void HandlePublisherReconnecting(int32 Attempt);
	UFUNCTION()
	void HandlePublisherError(const FString& Message);

	void SetupBeep();
	void ApplyInputMode();

	UPROPERTY(Transient)
	TObjectPtr<URTMPServer> Server;

	UPROPERTY(Transient)
	TObjectPtr<UIPRtmpReceiver> Receiver;

	UPROPERTY(Transient)
	TObjectPtr<UIPRtmpPublisher> Publisher;

	UPROPERTY(Transient)
	TObjectPtr<UIPRtmpExampleWidget> Panel;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ScreenMID;

	UPROPERTY(Transient)
	TObjectPtr<USoundWaveProcedural> BeepWave;

	FString ReceiverStatus = TEXT("Idle");
	FString PublisherStatus = TEXT("Idle");
	bool bPanelFocused = true;
	double NextFlashTime = 0.0;
	/** Set on the Game Thread, consumed by the audio thread: start a 100 ms tone now. */
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> BeepRequest = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
};
