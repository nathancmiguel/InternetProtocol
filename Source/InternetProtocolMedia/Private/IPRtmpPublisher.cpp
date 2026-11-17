/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPRtmpPublisher.h"
#include "AudioDevice.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Publish/IPPublishPipeline.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Slate/SlateViewportProvider.h"
#include "Sound/SoundSubmix.h"
#include "TextureResource.h"
#include "Widgets/SWindow.h"
#include "rtmp/rtmppublish.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPRtmpPublisherObject, Log, All);

namespace IPMedia {
	/**
	 * Captures the game window's back buffer right before it is presented (render thread). The Game Thread only
	 * requests a capture (with its timestamp); the hook never touches UObjects.
	 */
	class FViewportCaptureHook : public TSharedFromThis<FViewportCaptureHook, ESPMode::ThreadSafe> {
	public:
		FViewportCaptureHook(TSharedRef<FPublishPipeline, ESPMode::ThreadSafe> InPipeline, const SWindow* InWindow)
			: Pipeline(MoveTemp(InPipeline)), Window(InWindow) {}

		void Register() {
			Handle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddThreadSafeSP(AsShared(), &FViewportCaptureHook::OnBackBuffer);
		}

		void Unregister() {
			if (Handle.IsValid() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer()) {
				FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(Handle);
			}
			Handle.Reset();
		}

		void Request(int64 TimestampMs) { Requested.store(TimestampMs); }

	private:
		void OnBackBuffer(SWindow& InWindow, ISlateViewportProvider& Provider) {
			// Only the address is compared: the window is never dereferenced here.
			if (Window && &InWindow != Window) {
				return;
			}
			const int64 Timestamp = Requested.exchange(-1);
			if (Timestamp < 0) {
				return;
			}
			if (FRHITexture* BackBuffer = Provider.GetBackBufferResource()) {
				if (Pipeline->GetCapturePool()->Capture(FRHICommandListImmediate::Get(), BackBuffer, Timestamp, ERHIAccess::Present)) {
					Pipeline->Wake();
				}
			}
		}

		TSharedRef<FPublishPipeline, ESPMode::ThreadSafe> Pipeline;
		const SWindow* Window;
		FDelegateHandle Handle;
		std::atomic<int64> Requested{-1};
	};
}

bool UIPRtmpPublisher::Start(const FString& Url, const FString& StreamKey, const FIPRtmpPublishConfig& InConfig) {
	Stop();
	Config = InConfig;
	// Encoders want even dimensions.
	Config.Width = FMath::Max(16, Config.Width & ~1);
	Config.Height = FMath::Max(16, Config.Height & ~1);
	Config.FrameRate = FMath::Clamp(Config.FrameRate, 1, 120);

	switch (Config.VideoSource) {
	case EIPPublishVideoSource::RenderTarget:
		if (!IsValid(Config.RenderTarget)) {
			Fail(TEXT("VideoSource is RenderTarget but no render target was set"));
			return false;
		}
		break;
	case EIPPublishVideoSource::SceneCapture:
		if (!IsValid(Config.SceneCapture)) {
			Fail(TEXT("VideoSource is SceneCapture but no scene capture component was set"));
			return false;
		}
		break;
	case EIPPublishVideoSource::Viewport:
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer()) {
			Fail(TEXT("viewport capture needs a Slate renderer (not available in this process)"));
			return false;
		}
		break;
	}

	bool bAudio = Config.AudioSource != EIPPublishAudioSource::None;
	if (Config.AudioSource == EIPPublishAudioSource::Submix) {
		AudioDevice = GEngine ? GEngine->GetMainAudioDevice() : FAudioDeviceHandle();
		if (!AudioDevice.IsValid()) {
			OnError.Broadcast(TEXT("no audio device (audio disabled?): publishing video only"));
			bAudio = false;
		}
	}

	IPMedia::FPublishPipeline::FSettings Settings;
	Settings.Video.Width = Config.Width;
	Settings.Video.Height = Config.Height;
	Settings.Video.FrameRate = Config.FrameRate;
	Settings.Video.BitrateKbps = Config.VideoBitrateKbps;
	Settings.Video.KeyframeIntervalSeconds = Config.KeyframeIntervalSeconds;
	Settings.bAudio = bAudio;
	Settings.Audio.SampleRate = IPMedia::FAudioRing::OutputRate;
	Settings.Audio.NumChannels = IPMedia::FAudioRing::OutputChannels;
	Settings.Audio.BitrateKbps = Config.AudioBitrateKbps;
	Settings.Network.bReconnect = Config.bReconnect;
	Settings.Network.MaxReconnectAttempts = Config.MaxReconnectAttempts;

	Pipeline = MakeShared<IPMedia::FPublishPipeline, ESPMode::ThreadSafe>(Settings);
	FString Error;
	if (!Pipeline->Start(Url, StreamKey, Error)) {
		Pipeline.Reset();
		AudioDevice = FAudioDeviceHandle();
		Fail(FString::Printf(TEXT("cannot publish to %s: %s"), *IRtmpPublishSession::MaskUrl(Url), *Error));
		return false;
	}

	if (bAudio && Config.AudioSource == EIPPublishAudioSource::Submix) {
		USoundSubmix& Submix = Config.Submix ? *Config.Submix : AudioDevice->GetMainSubmixObject();
		SubmixListener = MakeShared<IPMedia::FSubmixCapture, ESPMode::ThreadSafe>(Pipeline->GetAudioRing());
		AudioDevice->RegisterSubmixBufferListener(SubmixListener.ToSharedRef(), Submix);
		ListenedSubmix = &Submix;
	}
	if (Config.VideoSource == EIPPublishVideoSource::Viewport) {
		const SWindow* Window = nullptr;
		if (GEngine && GEngine->GameViewport) {
			Window = GEngine->GameViewport->GetWindow().Get();
		}
		ViewportHook = MakeShared<IPMedia::FViewportCaptureHook, ESPMode::ThreadSafe>(Pipeline.ToSharedRef(), Window);
		ViewportHook->Register();
	}

	UE_LOG(LogIPRtmpPublisherObject, Log, TEXT("Publishing %dx%d@%d to %s"), Config.Width, Config.Height, Config.FrameRate, *IRtmpPublishSession::MaskUrl(Url));
	NextCaptureTime = 0.0;
	SetState(EIPRtmpPublisherState::Connecting);
	return true;
}

void UIPRtmpPublisher::Stop() {
	if (!Pipeline.IsValid()) {
		return;
	}
	Shutdown();
	SetState(EIPRtmpPublisherState::Idle);
}

void UIPRtmpPublisher::Shutdown() {
	if (SubmixListener.IsValid()) {
		if (AudioDevice.IsValid() && ListenedSubmix.IsValid()) {
			AudioDevice->UnregisterSubmixBufferListener(SubmixListener.ToSharedRef(), *ListenedSubmix.Get());
		}
		SubmixListener.Reset();
	}
	ListenedSubmix.Reset();
	AudioDevice = FAudioDeviceHandle();
	if (ViewportHook.IsValid()) {
		ViewportHook->Unregister();
		ViewportHook.Reset();
	}
	if (Pipeline.IsValid()) {
		// Non-blocking: the encode thread finishes on its own (graceful unpublish, encoders closed there).
		Pipeline->Stop();
		Pipeline.Reset();
	}
}

void UIPRtmpPublisher::SetState(EIPRtmpPublisherState NewState) {
	if (NewState == State) {
		return;
	}
	State = NewState;
	OnStateChanged.Broadcast(State);
	if (State == EIPRtmpPublisherState::Publishing) {
		OnPublishing.Broadcast();
	}
}

void UIPRtmpPublisher::Fail(const FString& Message) {
	UE_LOG(LogIPRtmpPublisherObject, Warning, TEXT("RTMP publisher: %s"), *Message);
	OnError.Broadcast(Message);
	Shutdown();
	SetState(EIPRtmpPublisherState::Error);
}

FIPRtmpPublisherStats UIPRtmpPublisher::GetStats() const {
	FIPRtmpPublisherStats Stats = Pipeline.IsValid() ? Pipeline->GetStats() : FIPRtmpPublisherStats();
	Stats.State = State;
	return Stats;
}

void UIPRtmpPublisher::PushAudioSamples(const TArray<float>& Interleaved, int32 NumChannels, int32 SampleRate) {
	if (NumChannels > 0) {
		PushAudio(Interleaved.GetData(), Interleaved.Num() / NumChannels, NumChannels, SampleRate);
	}
}

void UIPRtmpPublisher::PushAudio(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate) {
	// Copy the pointer: Stop() on the Game Thread may reset the member concurrently.
	TSharedPtr<IPMedia::FPublishPipeline, ESPMode::ThreadSafe> Current = Pipeline;
	if (Current.IsValid() && Config.AudioSource == EIPPublishAudioSource::Manual) {
		Current->GetAudioRing()->Write(Interleaved, NumFrames, NumChannels, SampleRate);
	}
}

void UIPRtmpPublisher::CaptureFrame(int64 TimestampMs) {
	switch (Config.VideoSource) {
	case EIPPublishVideoSource::Viewport:
		if (ViewportHook.IsValid()) {
			ViewportHook->Request(TimestampMs);
		}
		return;
	case EIPPublishVideoSource::SceneCapture: {
		USceneCaptureComponent2D* Capture = Config.SceneCapture;
		if (!IsValid(Capture)) {
			Fail(TEXT("the scene capture component was destroyed"));
			return;
		}
		if (!Capture->TextureTarget) {
			if (!OwnedTarget) {
				OwnedTarget = NewObject<UTextureRenderTarget2D>(this);
				OwnedTarget->RenderTargetFormat = RTF_RGBA8;
				OwnedTarget->InitAutoFormat(Config.Width, Config.Height);
				OwnedTarget->UpdateResourceImmediate(true);
			}
			Capture->TextureTarget = OwnedTarget;
		}
		if (!Capture->bCaptureEveryFrame) {
			Capture->CaptureScene();
		}
		Config.RenderTarget = Capture->TextureTarget;
		break;
	}
	case EIPPublishVideoSource::RenderTarget:
		break;
	}
	UTextureRenderTarget2D* Target = Config.RenderTarget;
	if (!IsValid(Target)) {
		Fail(TEXT("the render target was destroyed"));
		return;
	}
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	if (!Resource) {
		return;
	}
	// The target stays referenced by Config while publishing, and its resource is released by a render command
	// queued after this one, so the pointer is valid when the command runs.
	ENQUEUE_RENDER_COMMAND(IPRtmpPublishCapture)([Current = Pipeline, Resource, TimestampMs](FRHICommandListImmediate& RHICmdList) {
		FRHITexture* Texture = Resource->GetRenderTargetTexture();
		if (Texture && Current->GetCapturePool()->Capture(RHICmdList, Texture, TimestampMs, ERHIAccess::SRVMask)) {
			Current->Wake();
		}
	});
}

void UIPRtmpPublisher::Tick(float DeltaTime) {
	if (!Pipeline.IsValid()) {
		return;
	}
	IPMedia::FPublishPipeline::FEvent Event;
	while (Pipeline.IsValid() && Pipeline->PollEvent(Event)) {
		switch (Event.Kind) {
		case IPMedia::FPublishPipeline::FEvent::EKind::State:
			if (Event.State == EIPRtmpPublisherState::Error) {
				// Unrecoverable (encoder missing, rejected stream key, reconnection attempts exhausted).
				Shutdown();
			}
			SetState(Event.State);
			break;
		case IPMedia::FPublishPipeline::FEvent::EKind::Error:
			OnError.Broadcast(Event.Message);
			break;
		case IPMedia::FPublishPipeline::FEvent::EKind::Reconnecting:
			OnReconnecting.Broadcast(Event.Attempt);
			break;
		}
	}
	if (!Pipeline.IsValid() || !Pipeline->IsCapturing()) {
		return;
	}
	// Capture clock: at most one frame per game frame, at the configured rate.
	const double Now = FPlatformTime::Seconds();
	const double Interval = 1.0 / Config.FrameRate;
	if (Now + 0.002 < NextCaptureTime) {
		return;
	}
	NextCaptureTime = (Now - NextCaptureTime > Interval) ? Now + Interval : NextCaptureTime + Interval;
	CaptureFrame(Pipeline->NowMs());
}

TStatId UIPRtmpPublisher::GetStatId() const {
	RETURN_QUICK_DECLARE_CYCLE_STAT(UIPRtmpPublisher, STATGROUP_Tickables);
}

ETickableTickType UIPRtmpPublisher::GetTickableTickType() const {
	return HasAnyFlags(RF_ClassDefaultObject) ? ETickableTickType::Never : ETickableTickType::Conditional;
}

void UIPRtmpPublisher::BeginDestroy() {
	// No events: the object is going away. Unregistering and stopping never block.
	Shutdown();
	Super::BeginDestroy();
}
