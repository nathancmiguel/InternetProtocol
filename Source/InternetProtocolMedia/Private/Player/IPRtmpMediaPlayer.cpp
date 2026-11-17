/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Player/IPRtmpMediaPlayer.h"
#include "Player/IPRtmpPipeline.h"
#include "Player/IPRtmpUrl.h"
#include "IMediaEventSink.h"
#include "IMediaOptions.h"
#include "IPRtmpMediaSource.h"
#include "MediaSamples.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPRtmpPlayer, Log, All);

#define LOCTEXT_NAMESPACE "IPRtmpMediaPlayer"

namespace IPMedia {
	const FGuid RtmpPlayerPluginGuid(0x5C3B8E2A, 0x7F144D6B, 0x9A213E8C, 0x0D4F7B19);

	FRtmpMediaPlayer::FRtmpMediaPlayer(IMediaEventSink& InEventSink) : EventSink(InEventSink) {
		// Few queued video frames (they are released just in time), enough audio blocks for the sound component.
		Samples = MakeShared<FMediaSamples, ESPMode::ThreadSafe>(128, 4);
	}

	FRtmpMediaPlayer::~FRtmpMediaPlayer() {
		// No events here: the facade may already be tearing down. Stop() never blocks.
		if (Pipeline.IsValid()) {
			Pipeline->Stop();
			Pipeline.Reset();
		}
	}

	bool FRtmpMediaPlayer::Open(const FString& InUrl, const IMediaOptions* Options) {
		Close();
		const bool bPull = IsPullUrl(InUrl);
		uint16 Port = 0;
		FString App, Key, Error;
		if (!bPull && !ParseIpRtmpUrl(InUrl, Port, App, Key, Error)) {
			UE_LOG(LogIPRtmpPlayer, Error, TEXT("Cannot open %s: %s"), *MaskIpRtmpUrl(InUrl), *Error);
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpenFailed);
			return false;
		}

		bool bLowLatency = false;
		int64 LatencyMs = 0;
		if (Options) {
			bLowLatency = Options->GetMediaOption(UIPRtmpMediaSource::LatencyModeOption, static_cast<int64>(EIPRtmpLatencyMode::Smooth)) ==
			              static_cast<int64>(EIPRtmpLatencyMode::LowLatency);
			LatencyMs = Options->GetMediaOption(UIPRtmpMediaSource::LatencyMsOption, static_cast<int64>(0));
		}
		const FPlayoutSettings Settings = bLowLatency ? FPlayoutSettings::LowLatency(LatencyMs > 0 ? static_cast<int32>(LatencyMs) : 150)
		                                              : FPlayoutSettings::Smooth(LatencyMs > 0 ? static_cast<int32>(LatencyMs) : 1000);

		Pipeline = MakeShared<FRtmpPipeline, ESPMode::ThreadSafe>(Port, App, Key, Settings, Samples.ToSharedRef());
		const FString PullKey = Options ? Options->GetMediaOption(UIPRtmpMediaSource::StreamKeyOption, FString()) : FString();
		if (bPull ? !Pipeline->StartPull(InUrl, PullKey, Error) : !Pipeline->Start(Error)) {
			UE_LOG(LogIPRtmpPlayer, Error, TEXT("Cannot open %s: %s"), *MaskIpRtmpUrl(InUrl), *Error);
			Pipeline.Reset();
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpenFailed);
			return false;
		}
		Url = InUrl;
		State = EMediaState::Stopped;
		Rate = 0.0f;
		UE_LOG(LogIPRtmpPlayer, Log, TEXT("%s %s"), bPull ? TEXT("Playing RTMP stream from") : TEXT("Waiting for RTMP publisher on"), *MaskIpRtmpUrl(InUrl));
		EventSink.ReceiveMediaEvent(EMediaEvent::MediaConnecting);
		EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
		EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpened);
		return true;
	}

	void FRtmpMediaPlayer::Close() {
		if (!Pipeline.IsValid()) {
			return;
		}
		Pipeline->Stop();
		Pipeline.Reset();
		if (IsInGameThread()) {
			Samples->FlushSamples();
		}
		State = EMediaState::Closed;
		Rate = 0.0f;
		Url.Reset();
		EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
		EventSink.ReceiveMediaEvent(EMediaEvent::MediaClosed);
	}

	void FRtmpMediaPlayer::TickInput(FTimespan DeltaTime, FTimespan Timecode) {
		if (!Pipeline.IsValid()) {
			return;
		}
		EMediaEvent Event;
		while (Pipeline->PollEvent(Event)) {
			if (Event == EMediaEvent::MediaBuffering) {
				bBuffering = true;
			} else if (Event == EMediaEvent::MediaBufferingComplete) {
				bBuffering = false;
			}
			EventSink.ReceiveMediaEvent(Event);
		}
		CachedStats = Pipeline->GetStats();
	}

	IMediaSamples& FRtmpMediaPlayer::GetSamples() {
		return *Samples;
	}

	bool FRtmpMediaPlayer::GetPlayerFeatureFlag(EFeatureFlag Flag) const {
		switch (Flag) {
		case EFeatureFlag::UsePlaybackTimingV2:
		// The pipeline releases each frame at its presentation time (live playout clock): the facade shows the
		// newest one instead of re-timing frames with its own estimate.
		case EFeatureFlag::AlwaysPullNewestVideoFrame:
		case EFeatureFlag::UseRealtimeWithVideoOnly:
		case EFeatureFlag::PlayerUsesInternalFlushOnSeek:
		case EFeatureFlag::IsTrackSwitchSeamless:
		case EFeatureFlag::PlayerSelectsDefaultTracks:
			return true;
		default:
			return false;
		}
	}

	FString FRtmpMediaPlayer::GetInfo() const {
		return FString::Printf(TEXT("InternetProtocol RTMP receiver\nVideo: %s %dx%d\nAudio: %s %d Hz %d ch"),
			*CachedStats.VideoDecoder, CachedStats.Width, CachedStats.Height, *CachedStats.AudioDecoder, CachedStats.AudioSampleRate, CachedStats.AudioChannels);
	}

	FString FRtmpMediaPlayer::GetStats() const {
		return FString::Printf(TEXT("fps %.1f, %.0f kbit/s, buffered %.0f ms, dropped video %lld audio %lld, rebuffers %d"),
			CachedStats.Fps, CachedStats.BitrateKbps, CachedStats.BufferedMs, CachedStats.DroppedVideoFrames, CachedStats.DroppedAudioFrames, CachedStats.Rebuffers);
	}

	FIPRtmpReceiverStats FRtmpMediaPlayer::GetReceiverStats() const {
		return Pipeline.IsValid() ? Pipeline->GetStats() : FIPRtmpReceiverStats();
	}

	FIPVideoFrameInfo FRtmpMediaPlayer::GetLastFrame(uint64& OutFrameCounter) const {
		OutFrameCounter = 0;
		return Pipeline.IsValid() ? Pipeline->GetLastFrame(OutFrameCounter) : FIPVideoFrameInfo();
	}

	bool FRtmpMediaPlayer::TakeError(FString& OutError) {
		return Pipeline.IsValid() && Pipeline->TakeError(OutError);
	}

	// ---- IMediaControls ----

	bool FRtmpMediaPlayer::CanControl(EMediaControl Control) const {
		return Control == EMediaControl::Pause || Control == EMediaControl::Resume;
	}

	EMediaStatus FRtmpMediaPlayer::GetStatus() const {
		return bBuffering ? EMediaStatus::Buffering : EMediaStatus::None;
	}

	TRangeSet<float> FRtmpMediaPlayer::GetSupportedRates(EMediaRateThinning Thinning) const {
		TRangeSet<float> Rates;
		Rates.Add(TRange<float>(0.0f));
		Rates.Add(TRange<float>(1.0f));
		return Rates;
	}

	FTimespan FRtmpMediaPlayer::GetTime() const {
		return Pipeline.IsValid() ? Pipeline->GetPosition() : FTimespan::Zero();
	}

	bool FRtmpMediaPlayer::SetRate(float InRate) {
		if (!Pipeline.IsValid() || (InRate != 0.0f && InRate != 1.0f)) {
			return false;
		}
		Rate = InRate;
		Pipeline->SetPaused(InRate == 0.0f);
		const EMediaState NewState = InRate == 0.0f ? EMediaState::Paused : EMediaState::Playing;
		if (NewState != State) {
			State = NewState;
			EventSink.ReceiveMediaEvent(InRate == 0.0f ? EMediaEvent::PlaybackSuspended : EMediaEvent::PlaybackResumed);
		}
		return true;
	}

	// ---- IMediaTracks ----

	int32 FRtmpMediaPlayer::GetNumTracks(EMediaTrackType TrackType) const {
		if (TrackType == EMediaTrackType::Video) {
			return CachedStats.VideoDecoder.IsEmpty() ? 0 : 1;
		}
		if (TrackType == EMediaTrackType::Audio) {
			return CachedStats.AudioDecoder.IsEmpty() ? 0 : 1;
		}
		return 0;
	}

	int32 FRtmpMediaPlayer::GetNumTrackFormats(EMediaTrackType TrackType, int32 TrackIndex) const {
		return TrackIndex == 0 && GetNumTracks(TrackType) > 0 ? 1 : 0;
	}

	int32 FRtmpMediaPlayer::GetSelectedTrack(EMediaTrackType TrackType) const {
		return GetNumTracks(TrackType) > 0 ? 0 : INDEX_NONE;
	}

	int32 FRtmpMediaPlayer::GetTrackFormat(EMediaTrackType TrackType, int32 TrackIndex) const {
		return GetNumTrackFormats(TrackType, TrackIndex) > 0 ? 0 : INDEX_NONE;
	}

	FText FRtmpMediaPlayer::GetTrackDisplayName(EMediaTrackType TrackType, int32 TrackIndex) const {
		return TrackType == EMediaTrackType::Video ? LOCTEXT("VideoTrack", "RTMP video (H.264)") : LOCTEXT("AudioTrack", "RTMP audio (AAC)");
	}

	FString FRtmpMediaPlayer::GetTrackName(EMediaTrackType TrackType, int32 TrackIndex) const {
		return TrackType == EMediaTrackType::Video ? TEXT("video") : TEXT("audio");
	}

	bool FRtmpMediaPlayer::GetAudioTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaAudioTrackFormat& OutFormat) const {
		if (TrackIndex != 0 || FormatIndex != 0 || CachedStats.AudioDecoder.IsEmpty()) {
			return false;
		}
		OutFormat.BitsPerSample = 32;
		OutFormat.NumChannels = static_cast<uint32>(CachedStats.AudioChannels);
		OutFormat.SampleRate = static_cast<uint32>(CachedStats.AudioSampleRate);
		OutFormat.TypeName = TEXT("AAC");
		return true;
	}

	bool FRtmpMediaPlayer::GetVideoTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaVideoTrackFormat& OutFormat) const {
		if (TrackIndex != 0 || FormatIndex != 0 || CachedStats.VideoDecoder.IsEmpty()) {
			return false;
		}
		OutFormat.Dim = FIntPoint(CachedStats.Width, CachedStats.Height);
		OutFormat.FrameRate = CachedStats.Fps;
		OutFormat.FrameRates = TRange<float>(CachedStats.Fps);
		OutFormat.TypeName = TEXT("H.264");
		return true;
	}

	bool FRtmpMediaPlayer::SelectTrack(EMediaTrackType TrackType, int32 TrackIndex) {
		return TrackIndex == 0 || TrackIndex == INDEX_NONE;
	}
}

#undef LOCTEXT_NAMESPACE
