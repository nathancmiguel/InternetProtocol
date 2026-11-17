/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPRtmpReceiver.h"
#include "IPRtmpMediaSource.h"
#include "GameFramework/Actor.h"
#include "MediaPlayer.h"
#include "MediaPlayerFacade.h"
#include "MediaSoundComponent.h"
#include "MediaTexture.h"
#include "Player/IPRtmpMediaPlayer.h"
#include "Player/IPRtmpUrl.h"
#include "rtmp/rtmpserver.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPRtmpReceiverObject, Log, All);

namespace {
	/** The native iprtmp:// player behind a UMediaPlayer, or nullptr (other player, not open yet). */
	IPMedia::FRtmpMediaPlayer* GetRtmpPlayer(const UMediaPlayer* MediaPlayer) {
		if (!MediaPlayer) {
			return nullptr;
		}
		TSharedPtr<IMediaPlayer, ESPMode::ThreadSafe> Player = MediaPlayer->GetPlayerFacade()->GetPlayer();
		if (!Player.IsValid() || Player->GetPlayerPluginGUID() != IPMedia::RtmpPlayerPluginGuid) {
			return nullptr;
		}
		return static_cast<IPMedia::FRtmpMediaPlayer*>(Player.Get());
	}
}

bool UIPRtmpReceiver::StartUrl(const FString& Url, EIPRtmpLatencyMode LatencyMode, int32 LatencyMs) {
	return StartPull(Url, FString(), LatencyMode, LatencyMs);
}

bool UIPRtmpReceiver::StartPull(const FString& Url, const FString& StreamKey, EIPRtmpLatencyMode LatencyMode, int32 LatencyMs) {
	Stop();
	uint16 Port;
	FString App, Key, Error;
	if (!IPMedia::IsPullUrl(Url) && !IPMedia::ParseIpRtmpUrl(Url, Port, App, Key, Error)) {
		OnError.Broadcast(Error);
		return false;
	}

	Source = NewObject<UIPRtmpMediaSource>(this);
	Source->SetStreamKey(StreamKey);
	Source->StreamUrl = Url;
	Source->LatencyMode = LatencyMode;
	Source->LatencyMs = FMath::Max(LatencyMs, 0);

	if (!MediaPlayer) {
		MediaPlayer = NewObject<UMediaPlayer>(this);
		MediaPlayer->PlayOnOpen = true;
		MediaPlayer->SetLooping(false);
	}
	if (!VideoTexture) {
		VideoTexture = NewObject<UMediaTexture>(this);
		VideoTexture->AutoClear = true;
		VideoTexture->SetMediaPlayer(MediaPlayer);
		VideoTexture->UpdateResource();
	}
	if (SoundComponent) {
		SoundComponent->SetMediaPlayer(MediaPlayer);
	}

	if (!MediaPlayer->OpenSource(Source)) {
		OnError.Broadcast(FString::Printf(TEXT("could not open %s"), *IPMedia::MaskIpRtmpUrl(Url)));
		return false;
	}
	bActive = true;
	bStreaming = false;
	LastFrameCounter = 0;
	return true;
}

bool UIPRtmpReceiver::StartFromServer(URTMPServer* Server, const FString& App, const FString& StreamKey, EIPRtmpLatencyMode LatencyMode, int32 LatencyMs) {
	if (!IsValid(Server) || !Server->IsOpen()) {
		OnError.Broadcast(TEXT("the RTMP server is not open"));
		return false;
	}
	const FString Url = FString::Printf(TEXT("iprtmp://127.0.0.1:%d/%s/%s"), Server->LocalEndpoint().Port, *App, *StreamKey);
	return StartUrl(Url, LatencyMode, LatencyMs);
}

void UIPRtmpReceiver::Stop() {
	if (!bActive) {
		return;
	}
	bActive = false;
	if (MediaPlayer) {
		MediaPlayer->Close();
	}
	if (bStreaming) {
		bStreaming = false;
		OnEnded.Broadcast();
	}
}

UMediaSoundComponent* UIPRtmpReceiver::CreateAudioComponent(AActor* Owner) {
	if (SoundComponent && IsValid(SoundComponent)) {
		return SoundComponent;
	}
	if (!IsValid(Owner)) {
		OnError.Broadcast(TEXT("CreateAudioComponent needs a valid owner actor"));
		return nullptr;
	}
	if (!MediaPlayer) {
		MediaPlayer = NewObject<UMediaPlayer>(this);
		MediaPlayer->PlayOnOpen = true;
	}
	SoundComponent = NewObject<UMediaSoundComponent>(Owner);
	SoundComponent->bAllowSpatialization = true;
	SoundComponent->SetMediaPlayer(MediaPlayer);
	SoundComponent->RegisterComponent();
	if (USceneComponent* Root = Owner->GetRootComponent()) {
		SoundComponent->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
	}
	return SoundComponent;
}

FIPRtmpReceiverStats UIPRtmpReceiver::GetStats() const {
	const IPMedia::FRtmpMediaPlayer* Player = GetRtmpPlayer(MediaPlayer);
	return Player ? Player->GetReceiverStats() : FIPRtmpReceiverStats();
}

void UIPRtmpReceiver::Tick(float DeltaTime) {
	IPMedia::FRtmpMediaPlayer* Player = GetRtmpPlayer(MediaPlayer);
	if (!Player) {
		return;
	}
	FString Error;
	while (Player->TakeError(Error)) {
		OnError.Broadcast(Error);
	}
	const FIPRtmpReceiverStats Stats = Player->GetReceiverStats();
	if (Stats.bStreaming != bStreaming) {
		bStreaming = Stats.bStreaming;
		if (bStreaming) {
			OnStarted.Broadcast();
		} else {
			OnEnded.Broadcast();
		}
	}
	uint64 Counter = 0;
	const FIPVideoFrameInfo Frame = Player->GetLastFrame(Counter);
	if (Counter != LastFrameCounter) {
		LastFrameCounter = Counter;
		OnFrame.Broadcast(Frame);
	}
}

TStatId UIPRtmpReceiver::GetStatId() const {
	RETURN_QUICK_DECLARE_CYCLE_STAT(UIPRtmpReceiver, STATGROUP_Tickables);
}

ETickableTickType UIPRtmpReceiver::GetTickableTickType() const {
	return HasAnyFlags(RF_ClassDefaultObject) ? ETickableTickType::Never : ETickableTickType::Conditional;
}

void UIPRtmpReceiver::BeginDestroy() {
	// Do not touch MediaPlayer here: it may be collected in the same GC pass and closes itself when destroyed.
	bActive = false;
	Super::BeginDestroy();
}
