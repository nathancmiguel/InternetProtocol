/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IMediaEventSink.h"
#include "IPMediaDecoders.h"
#include "IPMediaTypes.h"
#include "MediaSamples.h"
#include "Player/IPPlayoutClock.h"
#include "rtmp/rtmpmedia.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include <atomic>

/*
 * Receive pipeline of one iprtmp:// player:
 *
 *   network strand: IRtmpMediaSink -> bounded input queue (copy/enqueue only, never blocks)
 *   decode thread:  decoders (Electra) -> pending decoded samples -> FPlayoutClock -> FMediaSamples
 *   game thread:    the player polls events/stats; the Media Framework pulls samples
 *
 * The decode thread owns the decoders and keeps the pipeline alive (shared reference) until it exits, so
 * Stop() never blocks the Game Thread. Timestamps are re-based onto a continuous player timeline: a new
 * publish or a jump in the source timestamps continues where the timeline was instead of restarting.
 */
namespace IPMedia {

	class FRtmpPipeline : public IRtmpMediaSink, public TSharedFromThis<FRtmpPipeline, ESPMode::ThreadSafe> {
	public:
		FRtmpPipeline(uint16 InPort, FString InApp, FString InKey, const FPlayoutSettings& InSettings,
		              TSharedRef<FMediaSamples, ESPMode::ThreadSafe> InSamples);
		virtual ~FRtmpPipeline() override;

		/** Game Thread. Registers the sink and makes sure the port is listened on. */
		bool Start(FString& OutError);
		/** Game Thread, non-blocking. */
		void Stop();
		void SetPaused(bool bInPaused) { bPaused.store(bInPaused); }

		// IRtmpMediaSink (network strand)
		virtual void OnStreamStarted(const FRtmpStreamInfo& Info) override;
		virtual void OnMediaPacket(const FRtmpMediaPacket& Packet) override;
		virtual void OnStreamEnded() override;

		// Game Thread polling
		bool PollEvent(EMediaEvent& OutEvent) { return Events.Dequeue(OutEvent); }
		bool TakeError(FString& OutError);
		FIPRtmpReceiverStats GetStats() const;
		FIPVideoFrameInfo GetLastFrame(uint64& OutFrameCounter) const;
		FTimespan GetPosition() const { return FTimespan(PositionTicks.load()); }

		/** Number of decode threads still running (module shutdown waits for them, bounded). */
		static int32 GetRunningThreads();

	private:
		struct FInput {
			enum class EKind : uint8 { Started, Packet, Ended };
			EKind Kind = EKind::Packet;
			FRtmpMediaPacket Packet;
			FRtmpStreamInfo Info;
		};

		struct FDecodedVideo {
			TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> Sample;
			FTimespan Pts;
			FTimespan Duration;
			FIntPoint Dim;
		};

		struct FDecodedAudio {
			TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe> Sample;
			FTimespan Pts;
			FTimespan Duration;
		};

		void ThreadMain();
		void Process(FInput& Input);
		void ProcessVideo(const FRtmpMediaPacket& Packet);
		void ProcessAudio(const FRtmpMediaPacket& Packet);
		void PumpOutputs();
		void Release(double Now);
		void UpdateStats(double Now);
		FTimespan MapTime(int64 StreamMs, int32 TypeIndex);
		void ReportError(const FString& Message);
		void ResetDecoders();

		const uint16 Port;
		const FString App;
		const FString Key;
		TSharedRef<FMediaSamples, ESPMode::ThreadSafe> Samples;

		// Input queue (network strand -> decode thread).
		FCriticalSection InputLock;
		TArray<FInput> InputQueue;
		int64 InputBytes = 0;
		bool bDropVideoUntilKeyframe = false;
		FEvent* WakeEvent = nullptr;

		// Decode thread state.
		FPlayoutClock Clock;
		TSharedPtr<IIPVideoDecoder, ESPMode::ThreadSafe> VideoDecoder;
		TSharedPtr<IIPAudioDecoder, ESPMode::ThreadSafe> AudioDecoder;
		bool bVideoConfigured = false;
		bool bAudioConfigured = false;
		bool bWaitKeyframe = true;
		int32 AudioSampleRate = 0;
		int32 AudioChannels = 0;
		double MetadataFps = 0.0;
		int32 MetadataWidth = 0;
		int32 MetadataHeight = 0;
		int64 LastVideoDtsMs = 0;
		int64 LastVideoFrameMs = 33;
		TArray<FDecodedVideo> PendingVideo;
		TArray<FDecodedAudio> PendingAudio;
		bool bHasAudioTrack = false;
		bool bHasVideoTrack = false;
		bool bAnchored = false;
		int64 TimelineOffsetMs = 0;
		int64 LastStreamMs[2] = {0, 0};
		bool bHaveLast[2] = {false, false};
		int64 LastTimelineMs = -40;
		bool bWasPaused = false;

		// Cross-thread.
		int64 SinkHandle = 0;
		int64 PortHandle = 0;
		std::atomic<bool> bStop{false};
		std::atomic<bool> bPaused{false};
		std::atomic<int64> PositionTicks{0};
		std::atomic<int64> ReceivedBytes{0};
		TQueue<EMediaEvent, EQueueMode::Mpsc> Events;

		mutable FCriticalSection StatsLock;
		FIPRtmpReceiverStats Stats;
		FIPVideoFrameInfo LastFrame;
		uint64 FrameCounter = 0;
		FString PendingError;
		double StatsWindowStart = 0.0;
		int32 FramesInWindow = 0;
		int64 BytesAtWindowStart = 0;
	};
}
