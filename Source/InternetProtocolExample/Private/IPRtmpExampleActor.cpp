/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPRtmpExampleActor.h"
#include "Components/AudioComponent.h"
#include "Components/InputComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "IPRtmpExampleWidget.h"
#include "IPRtmpPublisher.h"
#include "IPRtmpReceiver.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MediaSoundComponent.h"
#include "MediaTexture.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/ConstructorHelpers.h"
#include "rtmp/rtmpserver.h"

namespace {
	constexpr int32 BeepRate = 48000;
	constexpr double BeepSeconds = 0.1;
	constexpr double BeepHz = 1000.0;
}

AIPRtmpExampleActor::AIPRtmpExampleActor() {
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));

	// 16:9 screen: a thin box (visible from any side) whose -X face shows the video, towards the player start.
	Screen = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Screen"));
	Screen->SetupAttachment(RootComponent);
	Screen->SetStaticMesh(CubeMesh.Object);
	Screen->SetRelativeLocation(FVector(0.0, 0.0, 280.0));
	Screen->SetRelativeScale3D(FVector(0.05, 6.4, 3.6));
	Screen->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Something moving in front of the camera, to judge smoothness of the published stream.
	Spinner = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Spinner"));
	Spinner->SetupAttachment(RootComponent);
	Spinner->SetStaticMesh(CubeMesh.Object);
	Spinner->SetRelativeLocation(FVector(-400.0, -450.0, 100.0));
	Spinner->SetRelativeScale3D(FVector(0.8));

	// Flashes once per second together with the beep (A/V sync check).
	SyncLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("SyncLight"));
	SyncLight->SetupAttachment(Spinner);
	SyncLight->SetRelativeLocation(FVector(0.0, 0.0, 150.0));
	SyncLight->SetIntensity(0.0f);
	SyncLight->SetAttenuationRadius(800.0f);
	SyncLight->SetLightColor(FLinearColor(1.0f, 0.2f, 0.1f));

	SyncBeep = CreateDefaultSubobject<UAudioComponent>(TEXT("SyncBeep"));
	SyncBeep->SetupAttachment(Spinner);
	SyncBeep->bAutoActivate = false;

	// The publisher's "SceneCapture" source: looks at the spinner and the screen.
	Camera = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->SetRelativeLocation(FVector(-1200.0, -600.0, 350.0));
	Camera->SetRelativeRotation(FRotator(-8.0, 25.0, 0.0));
	Camera->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Camera->bCaptureEveryFrame = false;
	Camera->bCaptureOnMovement = false;
}

void AIPRtmpExampleActor::BeginPlay() {
	Super::BeginPlay();

	Receiver = NewObject<UIPRtmpReceiver>(this);
	Receiver->OnStarted.AddDynamic(this, &AIPRtmpExampleActor::HandleReceiverStarted);
	Receiver->OnEnded.AddDynamic(this, &AIPRtmpExampleActor::HandleReceiverEnded);
	Receiver->OnError.AddDynamic(this, &AIPRtmpExampleActor::HandleReceiverError);

	Publisher = NewObject<UIPRtmpPublisher>(this);
	Publisher->OnStateChanged.AddDynamic(this, &AIPRtmpExampleActor::HandlePublisherState);
	Publisher->OnReconnecting.AddDynamic(this, &AIPRtmpExampleActor::HandlePublisherReconnecting);
	Publisher->OnError.AddDynamic(this, &AIPRtmpExampleActor::HandlePublisherError);

	if (ScreenMaterial) {
		ScreenMID = UMaterialInstanceDynamic::Create(ScreenMaterial, this);
		Screen->SetMaterial(0, ScreenMID);
	}
	SetupBeep();

	// Command-line switch for unattended demos/tests: start receiving with the panel defaults.
	if (FParse::Param(FCommandLine::Get(), TEXT("IPRtmpExampleAutoReceive"))) {
		StartReceiving(1935, TEXT("live"), TEXT("demo"), EIPRtmpLatencyMode::LowLatency, 0);
	}

	if (bShowPanel) {
		if (APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0)) {
			Panel = CreateWidget<UIPRtmpExampleWidget>(Controller, UIPRtmpExampleWidget::StaticClass());
			Panel->SetExample(this);
			Panel->AddToViewport();
			EnableInput(Controller);
			if (InputComponent) {
				InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &AIPRtmpExampleActor::TogglePanelFocus);
			}
			ApplyInputMode();
		}
	}
}

void AIPRtmpExampleActor::EndPlay(const EEndPlayReason::Type Reason) {
	// Never blocks: both stop asynchronously.
	StopPublishing();
	StopReceiving();
	if (SyncBeep) {
		SyncBeep->Stop();
	}
	if (BeepWave) {
		BeepWave->OnSoundWaveProceduralUnderflow.Unbind();
	}
	Super::EndPlay(Reason);
}

void AIPRtmpExampleActor::SetupBeep() {
	BeepWave = NewObject<USoundWaveProcedural>(this);
	BeepWave->SetSampleRate(BeepRate);
	BeepWave->NumChannels = 1;
	BeepWave->Duration = INDEFINITELY_LOOPING_DURATION;
	BeepWave->SoundGroup = SOUNDGROUP_Default;
	BeepWave->bLooping = false;
	// Runs on the audio render thread: only touches the shared atomic and its own phase.
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Request = BeepRequest;
	TSharedRef<int64> Remaining = MakeShared<int64>(0);
	TSharedRef<int64> Phase = MakeShared<int64>(0);
	BeepWave->OnSoundWaveProceduralUnderflow.BindLambda([Request, Remaining, Phase](USoundWaveProcedural* Wave, int32 SamplesRequired) {
		if (Request->exchange(false)) {
			*Remaining = static_cast<int64>(BeepRate * BeepSeconds);
		}
		TArray<int16> Pcm;
		Pcm.SetNumZeroed(SamplesRequired);
		for (int32 Index = 0; Index < SamplesRequired && *Remaining > 0; ++Index, --*Remaining, ++*Phase) {
			Pcm[Index] = static_cast<int16>(9000.0 * FMath::Sin(2.0 * PI * BeepHz * (*Phase) / BeepRate));
		}
		Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm.GetData()), Pcm.Num() * sizeof(int16));
	});
	SyncBeep->SetSound(BeepWave);
	SyncBeep->Play();
}

void AIPRtmpExampleActor::Tick(float DeltaSeconds) {
	Super::Tick(DeltaSeconds);
	Spinner->AddLocalRotation(FRotator(0.0, 90.0 * DeltaSeconds, 45.0 * DeltaSeconds));

	const double Now = FPlatformTime::Seconds();
	if (Now >= NextFlashTime) {
		NextFlashTime = (Now - NextFlashTime > 1.0) ? Now + 1.0 : NextFlashTime + 1.0;
		SyncLight->SetIntensity(50000.0f);
		BeepRequest->store(true);
	} else if (Now > NextFlashTime - 0.9) {
		SyncLight->SetIntensity(0.0f);
	}
}

bool AIPRtmpExampleActor::StartReceiving(int32 Port, const FString& App, const FString& StreamKey, EIPRtmpLatencyMode Mode, int32 LatencyMs, const FString& ViewKey) {
	StopReceiving();
	if (!Server) {
		Server = NewObject<URTMPServer>(this);
	}
	Server->ClearPlaybackKeys();
	const bool bServeViewers = !ViewKey.IsEmpty() && Server->SetPlaybackKey(App, StreamKey, ViewKey);
	Server->SetPlaybackEnabled(bServeViewers, 0);
	if (!ViewKey.IsEmpty() && !bServeViewers) {
		ReceiverStatus = TEXT("The view key must differ from the stream key");
		return false;
	}
	FServerBindOptions Bind;
	Bind.Address = TEXT("0.0.0.0");
	Bind.Port = Port;
	if (!Server->Open(Bind)) {
		ReceiverStatus = FString::Printf(TEXT("Could not listen on port %d (in use?)"), Port);
		return false;
	}
	if (!Receiver->StartFromServer(Server, App, StreamKey, Mode, LatencyMs)) {
		Server->Close();
		return false;
	}
	BindScreen();
	ReceiverStatus = FString::Printf(TEXT("Waiting for a publisher on rtmp://<this machine>:%d/%s/***"), Port, *App);
	return true;
}

bool AIPRtmpExampleActor::StartPulling(const FString& Url, const FString& StreamKey, EIPRtmpLatencyMode Mode, int32 LatencyMs) {
	StopReceiving();
	if (!Receiver->StartPull(Url, StreamKey, Mode, LatencyMs)) {
		return false;
	}
	BindScreen();
	ReceiverStatus = TEXT("Connecting to the RTMP server...");
	return true;
}

void AIPRtmpExampleActor::BindScreen() {
	if (ScreenMID) {
		ScreenMID->SetTextureParameterValue(ScreenTextureParameter, Receiver->GetVideoTexture());
	}
	if (UMediaSoundComponent* Sound = Receiver->CreateAudioComponent(this)) {
		// Spatialized at the screen.
		Sound->AttachToComponent(Screen, FAttachmentTransformRules::KeepRelativeTransform);
	}
}

int32 AIPRtmpExampleActor::GetViewerCount() const {
	return Server && Server->IsOpen() ? Server->GetViewerCount(FString()) : 0;
}

void AIPRtmpExampleActor::StopReceiving() {
	if (Receiver) {
		Receiver->Stop();
	}
	if (Server && Server->IsOpen()) {
		Server->Close();
	}
	ReceiverStatus = TEXT("Idle");
}

bool AIPRtmpExampleActor::StartPublishing(const FString& Url, const FString& StreamKey, EIPPublishVideoSource Source, int32 Width, int32 Height,
                                          int32 FrameRate, int32 BitrateKbps, bool bAudio) {
	FIPRtmpPublishConfig Config;
	Config.VideoSource = Source;
	Config.SceneCapture = Camera;
	Config.Width = Width;
	Config.Height = Height;
	Config.FrameRate = FrameRate;
	Config.VideoBitrateKbps = BitrateKbps;
	Config.AudioSource = bAudio ? EIPPublishAudioSource::Submix : EIPPublishAudioSource::None;
	if (Source == EIPPublishVideoSource::RenderTarget) {
		// The demo has no render target of its own: use the camera's.
		if (!Camera->TextureTarget) {
			UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(this);
			Target->RenderTargetFormat = RTF_RGBA8;
			Target->InitAutoFormat(Width, Height);
			Camera->TextureTarget = Target;
		}
		Camera->bCaptureEveryFrame = true;
		Config.RenderTarget = Camera->TextureTarget;
	}
	PublisherStatus = TEXT("Connecting...");
	return Publisher->Start(Url, StreamKey, Config);
}

void AIPRtmpExampleActor::StopPublishing() {
	if (Publisher) {
		Publisher->Stop();
	}
	if (Camera) {
		Camera->bCaptureEveryFrame = false;
	}
	PublisherStatus = TEXT("Idle");
}

bool AIPRtmpExampleActor::IsReceiving() const {
	return Server && Server->IsOpen();
}

FIPRtmpReceiverStats AIPRtmpExampleActor::GetReceiverStats() const {
	return Receiver ? Receiver->GetStats() : FIPRtmpReceiverStats();
}

FIPRtmpPublisherStats AIPRtmpExampleActor::GetPublisherStats() const {
	return Publisher ? Publisher->GetStats() : FIPRtmpPublisherStats();
}

void AIPRtmpExampleActor::TogglePanelFocus() {
	bPanelFocused = !bPanelFocused;
	ApplyInputMode();
}

void AIPRtmpExampleActor::ApplyInputMode() {
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
	if (!Controller || !Panel) {
		return;
	}
	if (bPanelFocused) {
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(Panel->TakeWidget());
		Mode.SetHideCursorDuringCapture(false);
		Controller->SetInputMode(Mode);
		Controller->SetShowMouseCursor(true);
		Controller->SetIgnoreLookInput(true);
	} else {
		Controller->SetInputMode(FInputModeGameOnly());
		Controller->SetShowMouseCursor(false);
		Controller->SetIgnoreLookInput(false);
	}
}

void AIPRtmpExampleActor::HandleReceiverStarted() {
	ReceiverStatus = TEXT("Receiving");
}

void AIPRtmpExampleActor::HandleReceiverEnded() {
	ReceiverStatus = Receiver && Receiver->IsActive() ? TEXT("Stream ended, waiting for it to come back") : TEXT("Idle");
}

void AIPRtmpExampleActor::HandleReceiverError(const FString& Message) {
	ReceiverStatus = FString::Printf(TEXT("Error: %s"), *Message);
}

void AIPRtmpExampleActor::HandlePublisherState(EIPRtmpPublisherState State) {
	switch (State) {
	case EIPRtmpPublisherState::Idle: PublisherStatus = TEXT("Idle"); break;
	case EIPRtmpPublisherState::Connecting: PublisherStatus = TEXT("Connecting..."); break;
	case EIPRtmpPublisherState::Publishing: PublisherStatus = TEXT("Publishing"); break;
	case EIPRtmpPublisherState::Reconnecting: PublisherStatus = TEXT("Reconnecting..."); break;
	case EIPRtmpPublisherState::Error:
		if (!PublisherStatus.StartsWith(TEXT("Error"))) {
			PublisherStatus = TEXT("Error");
		}
		break;
	}
}

void AIPRtmpExampleActor::HandlePublisherReconnecting(int32 Attempt) {
	PublisherStatus = FString::Printf(TEXT("Reconnecting (attempt %d)..."), Attempt);
}

void AIPRtmpExampleActor::HandlePublisherError(const FString& Message) {
	PublisherStatus = FString::Printf(TEXT("Error: %s"), *Message);
}
