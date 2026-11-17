/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IPMediaEncoders.h"
#include "IPRtmpPublishTypes.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "Publish/IPAudioCapture.h"
#include "Publish/IPVideoCapture.h"
#include "rtmp/rtmppublish.h"
#include <atomic>

/*
 * Publish pipeline of one UIPRtmpPublisher:
 *
 *   render thread:       capture draw -> FVideoCapturePool (shared textures + GPU fence)
 *   audio render thread: submix -> FAudioRing (lock-free, 1 s)
 *   encode thread:       fence done -> H.264 encoder -> AVCC/avcC ; ring -> AAC encoder -> raw AAC
 *                        -> IRtmpPublishSession (bounded queue, congestion drops, reconnection)
 *   game thread:         schedules captures, polls events/stats
 *
 * One clock: milliseconds since Start() (FPlatformTime). Video uses the capture time, audio the sample count
 * anchored on that clock (re-anchored if it drifts more than 200 ms, e.g. after an audio underrun).
 * The encode thread keeps the pipeline alive until it exits, so Stop() never blocks the Game Thread.
 * The stream key is only handed to the RTMP session and never stored or logged here.
 */
namespace IPMedia {

	class FPublishPipeline : public TSharedFromThis<FPublishPipeline, ESPMode::ThreadSafe> {
	public:
		struct FSettings {
			FIPVideoEncoderSettings Video;
			bool bAudio = true;
			FIPAudioEncoderSettings Audio;
			FRtmpPublishOptions Network;
		};

		struct FEvent {
			enum class EKind : uint8 { State, Error, Reconnecting };
			EKind Kind = EKind::State;
			EIPRtmpPublisherState State = EIPRtmpPublisherState::Idle;
			FString Message;
			int32 Attempt = 0;
		};

		explicit FPublishPipeline(const FSettings& InSettings);
		~FPublishPipeline();

		/** Game Thread. Validates the URL and starts the encode thread (which connects once encoders are open). */
		bool Start(const FString& Url, const FString& StreamKey, FString& OutError);
		/** Game Thread, non-blocking. */
		void Stop();

		/** Milliseconds on the publish clock. Any thread. */
		int64 NowMs() const;
		/** Wakes the encode thread (a frame was captured). Any thread. */
		void Wake();

		TSharedRef<FVideoCapturePool, ESPMode::ThreadSafe> GetCapturePool() const { return Pool; }
		TSharedRef<FAudioRing, ESPMode::ThreadSafe> GetAudioRing() const { return Ring; }

		bool PollEvent(FEvent& Out) { return Events.Dequeue(Out); }
		FIPRtmpPublisherStats GetStats() const;
		bool IsCapturing() const { return bPublishing.load() && !bStop.load(); }

		static int32 GetRunningThreads();

	private:
		void ThreadMain();
		void EncodeVideo();
		void EncodeAudio(double Now);
		void EncodeAudioBlock(const int16* Pcm, int64 TimestampMs);
		void UpdateStats(double Now);
		void PushError(const FString& Message);

		const FSettings Settings;
		const double ClockStart;
		TSharedRef<FVideoCapturePool, ESPMode::ThreadSafe> Pool;
		TSharedRef<FAudioRing, ESPMode::ThreadSafe> Ring;
		TSharedPtr<IRtmpPublishSession, ESPMode::ThreadSafe> Session;
		TQueue<FEvent, EQueueMode::Mpsc> Events;
		::FEvent* WakeEvent = nullptr;
		std::atomic<bool> bStop{false};
		std::atomic<bool> bPublishing{false};
		std::atomic<bool> bForceKeyframe{true};

		// Encode thread state.
		TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> VideoEncoder;
		TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> AudioEncoder;
		TArray<uint8> CurrentSps;
		TArray<uint8> CurrentPps;
		TArray<uint8> LastAvcC;
		bool bVideoConfigSent = false;
		int64 LastVideoTs = -1;
		bool bAudioAnchored = false;
		int64 AudioBaseMs = 0;
		int64 AudioFramesSinceBase = 0;
		int64 LastAudioTs = -1;
		double LastAudioDataTime = 0.0;
		TArray<int16> AudioBlock;

		// Stats.
		mutable FCriticalSection StatsLock;
		FIPRtmpPublisherStats Stats;
		double StatsWindowStart = 0.0;
		int32 FramesInWindow = 0;
		int64 BytesAtWindowStart = 0;
		int64 EncoderDroppedFrames = 0;
	};
}
