/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Publish/IPPublishPipeline.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Publish/IPAvcFormat.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformMisc.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogIPRtmpPublisher, Log, All);

namespace IPMedia {
	namespace {
		std::atomic<int32> GRunningPublishThreads{0};
		constexpr int32 AacFrameSamples = 1024;
		/** Audio timestamps are re-anchored on the publish clock when they drift further than this. */
		constexpr int64 AudioResyncMs = 200;
		/** Without audio input for this long, silence is sent so the audio track stays continuous. */
		constexpr double AudioSilenceAfter = 0.25;

		EIPRtmpPublisherState ToPublisherState(ERtmpPublishState State) {
			switch (State) {
			case ERtmpPublishState::Connecting: return EIPRtmpPublisherState::Connecting;
			case ERtmpPublishState::Publishing: return EIPRtmpPublisherState::Publishing;
			case ERtmpPublishState::Reconnecting: return EIPRtmpPublisherState::Reconnecting;
			case ERtmpPublishState::Failed: return EIPRtmpPublisherState::Error;
			default: return EIPRtmpPublisherState::Idle;
			}
		}
	}

	int32 FPublishPipeline::GetRunningThreads() {
		return GRunningPublishThreads.load();
	}

	FPublishPipeline::FPublishPipeline(const FSettings& InSettings)
		: Settings(InSettings),
		  ClockStart(FPlatformTime::Seconds()),
		  Pool(MakeShared<FVideoCapturePool, ESPMode::ThreadSafe>(InSettings.Video.Width, InSettings.Video.Height)),
		  Ring(MakeShared<FAudioRing, ESPMode::ThreadSafe>(FAudioRing::OutputRate)) {
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
		AudioBlock.SetNumZeroed(AacFrameSamples * FAudioRing::OutputChannels);
	}

	FPublishPipeline::~FPublishPipeline() {
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}

	int64 FPublishPipeline::NowMs() const {
		return static_cast<int64>((FPlatformTime::Seconds() - ClockStart) * 1000.0);
	}

	void FPublishPipeline::Wake() {
		if (WakeEvent) {
			WakeEvent->Trigger();
		}
	}

	bool FPublishPipeline::Start(const FString& Url, const FString& StreamKey, FString& OutError) {
		TWeakPtr<FPublishPipeline, ESPMode::ThreadSafe> Weak = AsShared();
		FRtmpPublishCallbacks Callbacks;
		Callbacks.OnStateChanged = [Weak](ERtmpPublishState State) {
			if (TSharedPtr<FPublishPipeline, ESPMode::ThreadSafe> Self = Weak.Pin()) {
				const bool bPublishing = State == ERtmpPublishState::Publishing;
				if (bPublishing && !Self->bPublishing.load()) {
					// New (re)connection: the server needs a keyframe to start decoding.
					Self->bForceKeyframe.store(true);
				}
				Self->bPublishing.store(bPublishing);
				FEvent Event;
				Event.Kind = FEvent::EKind::State;
				Event.State = ToPublisherState(State);
				Self->Events.Enqueue(MoveTemp(Event));
			}
		};
		Callbacks.OnError = [Weak](const FString& Message) {
			if (TSharedPtr<FPublishPipeline, ESPMode::ThreadSafe> Self = Weak.Pin()) {
				Self->PushError(Message);
			}
		};
		Callbacks.OnReconnecting = [Weak](int32 Attempt) {
			if (TSharedPtr<FPublishPipeline, ESPMode::ThreadSafe> Self = Weak.Pin()) {
				FEvent Event;
				Event.Kind = FEvent::EKind::Reconnecting;
				Event.Attempt = Attempt;
				Self->Events.Enqueue(MoveTemp(Event));
			}
		};
		FRtmpPublishOptions Options = Settings.Network;
		Options.Width = Settings.Video.Width;
		Options.Height = Settings.Video.Height;
		Options.FrameRate = Settings.Video.FrameRate;
		Options.VideoBitrateKbps = Settings.Video.BitrateKbps;
		if (Settings.bAudio) {
			Options.AudioSampleRate = Settings.Audio.SampleRate;
			Options.AudioChannels = Settings.Audio.NumChannels;
			Options.AudioBitrateKbps = Settings.Audio.BitrateKbps;
		}
		Session = IRtmpPublishSession::Create(Url, StreamKey, Options, MoveTemp(Callbacks), OutError);
		if (!Session.IsValid()) {
			return false;
		}
		{
			FScopeLock ScopeLock(&StatsLock);
			Stats.State = EIPRtmpPublisherState::Connecting;
		}
		GRunningPublishThreads.fetch_add(1);
		TSharedRef<FPublishPipeline, ESPMode::ThreadSafe> Self = AsShared();
		Async(EAsyncExecution::Thread, [Self]() {
			Self->ThreadMain();
			GRunningPublishThreads.fetch_sub(1);
		});
		return true;
	}

	void FPublishPipeline::Stop() {
		bStop.store(true);
		Wake();
	}

	void FPublishPipeline::PushError(const FString& Message) {
		UE_LOG(LogIPRtmpPublisher, Warning, TEXT("RTMP publisher: %s"), *Message);
		FEvent Event;
		Event.Kind = FEvent::EKind::Error;
		Event.Message = Message;
		Events.Enqueue(MoveTemp(Event));
	}

	void FPublishPipeline::ThreadMain() {
#if PLATFORM_WINDOWS
		// Media Foundation (AAC) needs COM on the thread that uses it.
		const bool bComInitialized = FWindowsPlatformMisc::CoInitialize();
#endif
		VideoEncoder = FIPMediaEncoders::CreateVideoEncoder();
		bool bReady = VideoEncoder.IsValid() && VideoEncoder->Open(Settings.Video);
		if (!bReady) {
			PushError(FString::Printf(TEXT("video encoder: %s"), VideoEncoder.IsValid() ? *VideoEncoder->GetLastError() : TEXT("not available")));
			FEvent Event;
			Event.Kind = FEvent::EKind::State;
			Event.State = EIPRtmpPublisherState::Error;
			Events.Enqueue(MoveTemp(Event));
		} else {
			if (Settings.bAudio) {
				AudioEncoder = FIPMediaEncoders::CreateAudioEncoder();
				if (AudioEncoder.IsValid() && AudioEncoder->Open(Settings.Audio)) {
					Session->SetAudioConfig(AudioEncoder->GetAudioSpecificConfig());
				} else {
					PushError(FString::Printf(TEXT("audio encoder: %s (publishing video only)"),
						AudioEncoder.IsValid() ? *AudioEncoder->GetLastError() : TEXT("not available")));
					AudioEncoder.Reset();
				}
			}
			{
				FScopeLock ScopeLock(&StatsLock);
				Stats.VideoEncoder = VideoEncoder->GetName();
				Stats.AudioEncoder = AudioEncoder.IsValid() ? AudioEncoder->GetName() : FString();
			}
			Session->Start();
			StatsWindowStart = FPlatformTime::Seconds();
			LastAudioDataTime = StatsWindowStart;
			while (!bStop.load()) {
				WakeEvent->Wait(FTimespan::FromMilliseconds(5));
				const double Now = FPlatformTime::Seconds();
				EncodeVideo();
				EncodeAudio(Now);
				UpdateStats(Now);
			}
			Session->Stop();
		}
		// Frames still waiting are simply released; encoders are closed on the thread that used them.
		FCapturedFrame Frame;
		while (Pool->TryTake(Frame)) {
			Pool->Release(Frame.Slot);
		}
		if (VideoEncoder.IsValid()) {
			VideoEncoder->Close();
			VideoEncoder.Reset();
		}
		if (AudioEncoder.IsValid()) {
			AudioEncoder->Close();
			AudioEncoder.Reset();
		}
		Session.Reset();
#if PLATFORM_WINDOWS
		if (bComInitialized) {
			FWindowsPlatformMisc::CoUninitialize();
		}
#endif
	}

	void FPublishPipeline::EncodeVideo() {
		FCapturedFrame Frame;
		while (Pool->TryTake(Frame)) {
			const bool bKey = bForceKeyframe.exchange(false);
			// Strictly increasing timestamps (two captures in the same millisecond cannot share one).
			const int64 Timestamp = FMath::Max(Frame.TimestampMs, LastVideoTs + 1);
			const bool bEncoded = VideoEncoder->Encode(Frame.Texture, Timestamp, bKey);
			if (!bEncoded) {
				++EncoderDroppedFrames;
				if (bKey) {
					bForceKeyframe.store(true);
				}
				PushError(FString::Printf(TEXT("video encode failed: %s"), *VideoEncoder->GetLastError()));
			}
			FIPEncodedVideoFrame Out;
			while (VideoEncoder->Receive(Out)) {
				TArray<uint8> Avcc;
				if (!AnnexBToAvcc(Out.AnnexB, Avcc, CurrentSps, CurrentPps)) {
					continue;
				}
				if (Out.bKeyframe && CurrentSps.Num() && CurrentPps.Num()) {
					// (Re)send the sequence header when the parameter sets change (first IDR, resolution change).
					TArray<uint8> AvcC = BuildAvcC(CurrentSps, CurrentPps);
					if (!bVideoConfigSent || AvcC != LastAvcC) {
						Session->SetVideoConfig(AvcC);
						LastAvcC = MoveTemp(AvcC);
						bVideoConfigSent = true;
					}
				}
				if (!bVideoConfigSent) {
					continue;
				}
				LastVideoTs = FMath::Max(Out.TimestampMs, LastVideoTs + 1);
				// No B-frames: DTS == PTS, composition time 0.
				if (!Session->SendVideo(LastVideoTs, 0, Out.bKeyframe, MoveTemp(Avcc))) {
					// Dropped by the session (congestion / not publishing): counted in its stats.
				}
				++FramesInWindow;
			}
			// The encoder has consumed the texture (low-latency mode returns the picture synchronously).
			Pool->Release(Frame.Slot);
		}
	}

	void FPublishPipeline::EncodeAudioBlock(const int16* Pcm, int64 TimestampMs) {
		if (!AudioEncoder->Encode(Pcm, AacFrameSamples, TimestampMs)) {
			PushError(FString::Printf(TEXT("audio encode failed: %s"), *AudioEncoder->GetLastError()));
			return;
		}
		FIPEncodedAudioFrame Out;
		while (AudioEncoder->Receive(Out)) {
			LastAudioTs = FMath::Max(Out.TimestampMs, LastAudioTs + 1);
			Session->SendAudio(LastAudioTs, MoveTemp(Out.Data));
		}
	}

	void FPublishPipeline::EncodeAudio(double Now) {
		if (!AudioEncoder.IsValid()) {
			return;
		}
		const double FrameMs = 1000.0 / FAudioRing::OutputRate;
		while (Ring->Available() >= AacFrameSamples) {
			// Estimated publish-clock time of the first sample of this block: data arrives in real time, so the
			// newest sample is "now" and older ones are Available frames earlier.
			const int64 Estimated = NowMs() - static_cast<int64>(Ring->Available() * FrameMs);
			if (!Ring->Read(AudioBlock.GetData(), AacFrameSamples)) {
				break;
			}
			int64 Timestamp = AudioBaseMs + static_cast<int64>(AudioFramesSinceBase * FrameMs);
			if (!bAudioAnchored || FMath::Abs(Timestamp - Estimated) > AudioResyncMs) {
				// First block, or drift / gap (underrun, device change): re-anchor on the publish clock.
				AudioBaseMs = FMath::Max<int64>(Estimated, LastAudioTs + 1);
				AudioFramesSinceBase = 0;
				Timestamp = AudioBaseMs;
				bAudioAnchored = true;
			}
			AudioFramesSinceBase += AacFrameSamples;
			LastAudioDataTime = Now;
			EncodeAudioBlock(AudioBlock.GetData(), Timestamp);
		}
		// No input for a while (paused audio device, manual source idle): keep the track continuous with silence,
		// otherwise players stall waiting for audio.
		if (Now - LastAudioDataTime > AudioSilenceAfter && Ring->Available() == 0) {
			const int64 Target = NowMs() - static_cast<int64>(AudioSilenceAfter * 1000.0);
			if (!bAudioAnchored) {
				AudioBaseMs = Target;
				AudioFramesSinceBase = 0;
				bAudioAnchored = true;
			}
			FMemory::Memzero(AudioBlock.GetData(), AudioBlock.Num() * sizeof(int16));
			int32 Blocks = 0;
			while (AudioBaseMs + static_cast<int64>(AudioFramesSinceBase * FrameMs) < Target && Blocks++ < 50) {
				const int64 Timestamp = AudioBaseMs + static_cast<int64>(AudioFramesSinceBase * FrameMs);
				AudioFramesSinceBase += AacFrameSamples;
				EncodeAudioBlock(AudioBlock.GetData(), Timestamp);
			}
		}
	}

	void FPublishPipeline::UpdateStats(double Now) {
		const double Elapsed = Now - StatsWindowStart;
		if (Elapsed < 1.0) {
			return;
		}
		const FRtmpPublishStats Net = Session->GetStats();
		FScopeLock ScopeLock(&StatsLock);
		Stats.Fps = static_cast<float>(FramesInWindow / Elapsed);
		Stats.SentKbps = static_cast<float>((Net.BytesSent - BytesAtWindowStart) * 8.0 / 1000.0 / Elapsed);
		Stats.DroppedVideoFrames = Pool->GetDroppedFrames() + EncoderDroppedFrames + Net.DroppedVideoFrames;
		Stats.DroppedAudioFrames = Ring->GetDroppedFrames() / AacFrameSamples + Net.DroppedAudioFrames;
		Stats.QueuedBytes = Net.QueuedBytes;
		Stats.RttMs = static_cast<float>(Net.RttMs);
		Stats.ReconnectAttempts = Net.ReconnectAttempts;
		Stats.State = ToPublisherState(Session->GetState());
		FramesInWindow = 0;
		BytesAtWindowStart = Net.BytesSent;
		StatsWindowStart = Now;
	}

	FIPRtmpPublisherStats FPublishPipeline::GetStats() const {
		FScopeLock ScopeLock(&StatsLock);
		return Stats;
	}
}
