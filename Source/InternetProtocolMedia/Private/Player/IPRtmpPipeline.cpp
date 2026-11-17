/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Player/IPRtmpPipeline.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPRtmpReceiver, Log, All);

namespace IPMedia {
	namespace {
		constexpr int32 MaxInputItems = 600;
		constexpr int64 MaxInputBytes = 48 * 1024 * 1024;
		/** Decoded video frames kept ahead of presentation: each pins a decoder output surface (see header). */
		constexpr int32 MaxDecodedVideo = 6;
		constexpr int32 MaxPendingAudio = 600;
		/** Encoded packets kept in the jitter buffer (bounded independently of the latency target). */
		constexpr int32 MaxEncodedVideo = 1800;
		constexpr int32 MaxEncodedAudio = 3000;
		/** How far ahead of the playout position audio packets are decoded. */
		const FTimespan AudioDecodeLead = FTimespan::FromMilliseconds(250);
		/**
		 * A video packet this far behind the position is skipped together with the rest of its GOP. Smaller delays
		 * (e.g. after a catch-up jump) are decoded anyway, faster than real time, and only the late pictures are
		 * dropped: skipping to the next keyframe would lose up to a whole GOP for a short burst.
		 */
		const FTimespan VideoSkipThreshold = FTimespan::FromMilliseconds(2000);
		constexpr int64 DiscontinuityMs = 1000;
		constexpr uint8 CodecH264 = 7;
		constexpr uint8 CodecAac = 10;

		std::atomic<int32> GRunningThreads{0};

		bool ParseAsc(const TArray<uint8>& Asc, int32& OutRate, int32& OutChannels) {
			static const int32 Rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
			if (Asc.Num() < 2) {
				return false;
			}
			const uint32 Bits = (static_cast<uint32>(Asc[0]) << 24) | (static_cast<uint32>(Asc[1]) << 16) |
			                    (Asc.Num() > 2 ? static_cast<uint32>(Asc[2]) << 8 : 0) | (Asc.Num() > 3 ? Asc[3] : 0);
			const uint32 FrequencyIndex = (Bits >> 23) & 0x0F;
			if (FrequencyIndex == 15) {
				if (Asc.Num() < 5) {
					return false;
				}
				// Explicit 24-bit sample rate after the escape index: 5+4+24+4 bits over 5 bytes.
				const uint64 Wide = (static_cast<uint64>(Asc[0]) << 32) | (static_cast<uint64>(Asc[1]) << 24) | (static_cast<uint64>(Asc[2]) << 16) |
				                    (static_cast<uint64>(Asc[3]) << 8) | Asc[4];
				OutRate = static_cast<int32>((Wide >> 7) & 0xFFFFFF);
				OutChannels = static_cast<int32>((Wide >> 3) & 0x0F);
				return OutRate > 0;
			}
			if (FrequencyIndex >= UE_ARRAY_COUNT(Rates)) {
				return false;
			}
			OutRate = Rates[FrequencyIndex];
			OutChannels = static_cast<int32>((Bits >> 19) & 0x0F);
			return true;
		}
	}

	int32 FRtmpPipeline::GetRunningThreads() {
		return GRunningThreads.load();
	}

	FRtmpPipeline::FRtmpPipeline(uint16 InPort, FString InApp, FString InKey, const FPlayoutSettings& InSettings,
	                             TSharedRef<FMediaSamples, ESPMode::ThreadSafe> InSamples)
		: Port(InPort), App(MoveTemp(InApp)), Key(MoveTemp(InKey)), Samples(MoveTemp(InSamples)), Clock(InSettings) {
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	}

	FRtmpPipeline::~FRtmpPipeline() {
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}

	bool FRtmpPipeline::Start(FString& OutError) {
		PortHandle = FRtmpIngestHub::AcquirePort(Port, OutError);
		if (PortHandle == 0) {
			return false;
		}
		SinkHandle = FRtmpIngestHub::RegisterSink(Port, App, Key, AsShared());
		GRunningThreads.fetch_add(1);
		TSharedRef<FRtmpPipeline, ESPMode::ThreadSafe> Self = AsShared();
		Async(EAsyncExecution::Thread, [Self]() {
			Self->ThreadMain();
			GRunningThreads.fetch_sub(1);
		});
		return true;
	}

	void FRtmpPipeline::Stop() {
		if (SinkHandle) {
			FRtmpIngestHub::UnregisterSink(SinkHandle);
			SinkHandle = 0;
		}
		if (PortHandle) {
			FRtmpIngestHub::ReleasePort(PortHandle);
			PortHandle = 0;
		}
		bStop.store(true);
		if (WakeEvent) {
			WakeEvent->Trigger();
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// Network strand

	void FRtmpPipeline::OnStreamStarted(const FRtmpStreamInfo& Info) {
		FInput Input;
		Input.Kind = FInput::EKind::Started;
		Input.Info = Info;
		{
			FScopeLock ScopeLock(&InputLock);
			InputQueue.Add(MoveTemp(Input));
		}
		WakeEvent->Trigger();
	}

	void FRtmpPipeline::OnStreamEnded() {
		FInput Input;
		Input.Kind = FInput::EKind::Ended;
		{
			FScopeLock ScopeLock(&InputLock);
			InputQueue.Add(MoveTemp(Input));
		}
		WakeEvent->Trigger();
	}

	void FRtmpPipeline::OnMediaPacket(const FRtmpMediaPacket& Packet) {
		const int64 Size = Packet.Payload.IsValid() ? Packet.Payload->Num() : 0;
		ReceivedBytes.fetch_add(Size);
		{
			FScopeLock ScopeLock(&InputLock);
			const bool bVideo = Packet.Type == ERtmpPacketType::Video;
			if (bVideo && !Packet.bSequenceHeader) {
				if (Packet.bKeyframe) {
					bDropVideoUntilKeyframe = false;
				} else if (bDropVideoUntilKeyframe) {
					return;
				}
			}
			if (InputQueue.Num() >= MaxInputItems || InputBytes + Size > MaxInputBytes) {
				// The decode thread cannot keep up: drop this packet; for video, everything until the next keyframe.
				FScopeLock StatsScope(&StatsLock);
				if (bVideo) {
					bDropVideoUntilKeyframe = true;
					++Stats.DroppedVideoFrames;
				} else {
					++Stats.DroppedAudioFrames;
				}
				return;
			}
			FInput Input;
			Input.Packet = Packet;
			InputQueue.Add(MoveTemp(Input));
			InputBytes += Size;
		}
		WakeEvent->Trigger();
	}

	// ---------------------------------------------------------------------------------------------------------
	// Decode thread

	void FRtmpPipeline::ThreadMain() {
		TArray<FInput> Batch;
		while (!bStop.load()) {
			WakeEvent->Wait(FTimespan::FromMilliseconds(4));
			{
				FScopeLock ScopeLock(&InputLock);
				Batch = MoveTemp(InputQueue);
				InputQueue.Reset();
				InputBytes = 0;
			}
			for (FInput& Input : Batch) {
				if (bStop.load()) {
					break;
				}
				Process(Input);
			}
			Batch.Reset();
			const double Now = FPlatformTime::Seconds();
			UpdateClock(Now);
			DecodeDue(Now);
			PumpOutputs();
			Release(Now);
			UpdateStats(Now);
		}
		// Decoders are closed on the thread that used them.
		ResetDecoders();
		EncodedVideo.Reset();
		EncodedAudio.Reset();
		PendingVideo.Reset();
		PendingAudio.Reset();
	}

	void FRtmpPipeline::ResetDecoders() {
		if (VideoDecoder.IsValid()) {
			VideoDecoder->Close();
			VideoDecoder.Reset();
		}
		if (AudioDecoder.IsValid()) {
			AudioDecoder->Close();
			AudioDecoder.Reset();
		}
		bVideoConfigured = false;
		bAudioConfigured = false;
		bWaitKeyframe = true;
	}

	void FRtmpPipeline::ReportError(const FString& Message) {
		UE_LOG(LogIPRtmpReceiver, Warning, TEXT("RTMP receiver (port %u, app '%s'): %s"), Port, *App, *Message);
		FScopeLock ScopeLock(&StatsLock);
		PendingError = Message;
	}

	bool FRtmpPipeline::TakeError(FString& OutError) {
		FScopeLock ScopeLock(&StatsLock);
		if (PendingError.IsEmpty()) {
			return false;
		}
		OutError = MoveTemp(PendingError);
		PendingError.Reset();
		return true;
	}

	FTimespan FRtmpPipeline::MapTime(int64 StreamMs, int32 TypeIndex) {
		if (!bAnchored) {
			// Continue the timeline where it stopped (first stream: starts at 0).
			TimelineOffsetMs = (LastTimelineMs + 40) - StreamMs;
			bAnchored = true;
			bHaveLast[0] = bHaveLast[1] = false;
		} else if (bHaveLast[TypeIndex] && FMath::Abs(StreamMs - LastStreamMs[TypeIndex]) > DiscontinuityMs) {
			TimelineOffsetMs = (LastTimelineMs + 40) - StreamMs;
			FScopeLock ScopeLock(&StatsLock);
			++Stats.Discontinuities;
		}
		LastStreamMs[TypeIndex] = StreamMs;
		bHaveLast[TypeIndex] = true;
		const int64 TimelineMs = StreamMs + TimelineOffsetMs;
		LastTimelineMs = FMath::Max(LastTimelineMs, TimelineMs);
		return FTimespan::FromMilliseconds(static_cast<double>(TimelineMs));
	}

	void FRtmpPipeline::Process(FInput& Input) {
		switch (Input.Kind) {
		case FInput::EKind::Started:
			MetadataFps = Input.Info.FrameRate;
			MetadataWidth = static_cast<int32>(Input.Info.Width);
			MetadataHeight = static_cast<int32>(Input.Info.Height);
			bAnchored = false;
			bWaitKeyframe = true;
			{
				FScopeLock ScopeLock(&StatsLock);
				Stats.bStreaming = true;
			}
			Events.Enqueue(EMediaEvent::MediaBuffering);
			return;
		case FInput::EKind::Ended:
			// Buffered media is still played out; the next publish continues the timeline.
			bAnchored = false;
			bWaitKeyframe = true;
			{
				FScopeLock ScopeLock(&StatsLock);
				Stats.bStreaming = false;
			}
			return;
		case FInput::EKind::Packet:
			if (Input.Packet.Type == ERtmpPacketType::Video) {
				ProcessVideo(Input.Packet);
			} else {
				ProcessAudio(Input.Packet);
			}
			return;
		}
	}

	void FRtmpPipeline::ProcessVideo(const FRtmpMediaPacket& Packet) {
		if (!Packet.Payload.IsValid()) {
			return;
		}
		if (Packet.Codec != CodecH264) {
			ReportError(FString::Printf(TEXT("unsupported video codec id %d (only H.264 is supported)"), Packet.Codec));
			return;
		}
		if (Packet.bSequenceHeader) {
			if (!VideoDecoder.IsValid()) {
				VideoDecoder = FIPMediaDecoders::CreateVideoDecoder();
			}
			bVideoConfigured = VideoDecoder.IsValid() && VideoDecoder->Configure(*Packet.Payload, FMath::Max(MetadataWidth, 1920), FMath::Max(MetadataHeight, 1080));
			if (!bVideoConfigured) {
				ReportError(FString::Printf(TEXT("video decoder: %s"), VideoDecoder.IsValid() ? *VideoDecoder->GetLastError() : TEXT("not available")));
			} else {
				bHasVideoTrack = true;
				Events.Enqueue(EMediaEvent::TracksChanged);
				FScopeLock ScopeLock(&StatsLock);
				Stats.VideoDecoder = VideoDecoder->GetName();
			}
			bWaitKeyframe = true;
			return;
		}
		if (!bVideoConfigured || (bWaitKeyframe && !Packet.bKeyframe)) {
			return;
		}
		bWaitKeyframe = false;

		// Frame duration from consecutive timestamps, else from metadata.
		if (bHaveLast[0] && Packet.DtsMs > LastVideoDtsMs && Packet.DtsMs - LastVideoDtsMs < 200) {
			LastVideoFrameMs = Packet.DtsMs - LastVideoDtsMs;
		} else if (MetadataFps > 1.0) {
			LastVideoFrameMs = static_cast<int64>(1000.0 / MetadataFps);
		}
		LastVideoDtsMs = Packet.DtsMs;

		FEncoded Encoded;
		Encoded.Data = Packet.Payload;
		Encoded.Dts = MapTime(Packet.DtsMs, 0);
		Encoded.Pts = Encoded.Dts + FTimespan::FromMilliseconds(Packet.CtsMs);
		Encoded.Duration = FTimespan::FromMilliseconds(static_cast<double>(FMath::Max<int64>(LastVideoFrameMs, 1)));
		Encoded.bKeyframe = Packet.bKeyframe;
		if (EncodedVideo.Num() >= MaxEncodedVideo) {
			// Far more than any latency target: start over at the next keyframe.
			{
				FScopeLock ScopeLock(&StatsLock);
				Stats.DroppedVideoFrames += EncodedVideo.Num();
			}
			EncodedVideo.Reset();
			bSkipToKeyframe = true;
		}
		EncodedVideo.Add(MoveTemp(Encoded));
	}

	void FRtmpPipeline::ProcessAudio(const FRtmpMediaPacket& Packet) {
		if (!Packet.Payload.IsValid()) {
			return;
		}
		if (Packet.Codec != CodecAac) {
			ReportError(FString::Printf(TEXT("unsupported audio format %d (only AAC is supported)"), Packet.Codec));
			return;
		}
		if (Packet.bSequenceHeader) {
			int32 Rate = 0, Channels = 0;
			if (!ParseAsc(*Packet.Payload, Rate, Channels) || Channels <= 0) {
				ReportError(TEXT("invalid AudioSpecificConfig"));
				return;
			}
			if (!AudioDecoder.IsValid()) {
				AudioDecoder = FIPMediaDecoders::CreateAudioDecoder();
			}
			bAudioConfigured = AudioDecoder.IsValid() && AudioDecoder->Configure(*Packet.Payload, Rate, Channels);
			if (!bAudioConfigured) {
				ReportError(FString::Printf(TEXT("audio decoder: %s"), AudioDecoder.IsValid() ? *AudioDecoder->GetLastError() : TEXT("not available")));
				return;
			}
			AudioSampleRate = Rate;
			AudioChannels = Channels;
			bHasAudioTrack = true;
			Events.Enqueue(EMediaEvent::TracksChanged);
			FScopeLock ScopeLock(&StatsLock);
			Stats.AudioDecoder = AudioDecoder->GetName();
			Stats.AudioSampleRate = Rate;
			Stats.AudioChannels = Channels;
			return;
		}
		if (!bAudioConfigured || AudioSampleRate <= 0) {
			return;
		}
		// Hold audio until video starts at its first keyframe, so both begin together.
		if (bHasVideoTrack && bWaitKeyframe) {
			return;
		}
		FEncoded Encoded;
		Encoded.Data = Packet.Payload;
		Encoded.Dts = MapTime(Packet.DtsMs, 1);
		Encoded.Pts = Encoded.Dts;
		Encoded.Duration = FTimespan::FromSeconds(1024.0 / AudioSampleRate);
		Encoded.bKeyframe = true;
		if (EncodedAudio.Num() >= MaxEncodedAudio) {
			EncodedAudio.RemoveAt(0);
			FScopeLock ScopeLock(&StatsLock);
			++Stats.DroppedAudioFrames;
		}
		EncodedAudio.Add(MoveTemp(Encoded));
	}

	void FRtmpPipeline::UpdateClock(double Now) {
		if (bPaused.load()) {
			bWasPaused = true;
			return;
		}
		if (bWasPaused) {
			// Live streams cannot resume where they paused: rebuffer from the current data.
			bWasPaused = false;
			Clock.Reset();
		}
		// Audio is the master track when present (the sound component drives the A/V clock). Buffered media is
		// everything not yet presented: decoded samples plus encoded packets.
		bool bHasPending = false;
		FTimespan Earliest = FTimespan::MaxValue(), Latest = FTimespan::MinValue();
		auto Consider = [&](FTimespan Pts) {
			bHasPending = true;
			Earliest = FMath::Min(Earliest, Pts);
			Latest = FMath::Max(Latest, Pts);
		};
		if (bHasAudioTrack) {
			if (PendingAudio.Num()) { Consider(PendingAudio[0].Pts); Consider(PendingAudio.Last().Pts); }
			if (EncodedAudio.Num()) { Consider(EncodedAudio[0].Pts); Consider(EncodedAudio.Last().Pts); }
		} else {
			if (PendingVideo.Num()) { Consider(PendingVideo[0].Pts); Consider(PendingVideo.Last().Pts); }
			if (EncodedVideo.Num()) { Consider(EncodedVideo[0].Pts); Consider(EncodedVideo.Last().Pts); }
		}
		const bool bWasPlaying = Clock.IsPlaying();
		const int32 RebuffersBefore = Clock.GetRebufferCount();
		Clock.Update(Now, bHasPending, bHasPending ? Earliest : FTimespan(), bHasPending ? Latest : FTimespan());
		if (!bWasPlaying && Clock.IsPlaying()) {
			Events.Enqueue(EMediaEvent::MediaBufferingComplete);
		} else if (Clock.GetRebufferCount() != RebuffersBefore) {
			Events.Enqueue(EMediaEvent::MediaBuffering);
		}
		if (Clock.IsPlaying()) {
			PositionTicks.store(Clock.GetPosition(Now).GetTicks());
		}
	}

	void FRtmpPipeline::DecodeDue(double Now) {
		if (!Clock.IsPlaying()) {
			return;
		}
		const FTimespan Position = Clock.GetPosition(Now);

		// Video: skip what can no longer be shown (catch-up after a stall), restarting at a keyframe.
		while (EncodedVideo.Num() > 0 && (bSkipToKeyframe || EncodedVideo[0].Pts + VideoSkipThreshold < Position)) {
			if (EncodedVideo[0].bKeyframe && EncodedVideo[0].Pts + VideoSkipThreshold >= Position) {
				bSkipToKeyframe = false;
				break;
			}
			bSkipToKeyframe = true;
			EncodedVideo.RemoveAt(0, EAllowShrinking::No);
			FScopeLock ScopeLock(&StatsLock);
			++Stats.DroppedVideoFrames;
		}
		// Video is fed as long as few decoded frames wait: the decoder holds a whole reorder window (up to 16 frames
		// for H.264) before its first output, so a time-based lead would starve it. Decoded frames stay bounded.
		while (VideoDecoder.IsValid() && bVideoConfigured && !bSkipToKeyframe && EncodedVideo.Num() > 0 &&
		       PendingVideo.Num() < MaxDecodedVideo) {
			const FEncoded& Front = EncodedVideo[0];
			FIPAccessUnit Unit;
			Unit.Data = Front.Data->GetData();
			Unit.Size = Front.Data->Num();
			Unit.Dts = Front.Dts;
			Unit.Pts = Front.Pts;
			Unit.Duration = Front.Duration;
			Unit.bKeyframe = Front.bKeyframe;
			const EIPDecodeResult Result = VideoDecoder->Decode(Unit);
			if (Result == EIPDecodeResult::Busy) {
				// Retried on the next loop iteration, once outputs have been taken.
				break;
			}
			EncodedVideo.RemoveAt(0, EAllowShrinking::No);
			if (Result == EIPDecodeResult::Error) {
				ReportError(FString::Printf(TEXT("video decode failed: %s"), *VideoDecoder->GetLastError()));
				bSkipToKeyframe = true;
				break;
			}
			PumpOutputs();
		}

		while (AudioDecoder.IsValid() && bAudioConfigured && EncodedAudio.Num() > 0 && EncodedAudio[0].Pts <= Position + AudioDecodeLead) {
			const FEncoded& Front = EncodedAudio[0];
			if (Front.Pts + Front.Duration < Position) {
				// Too late to be heard: do not even decode it.
				EncodedAudio.RemoveAt(0, EAllowShrinking::No);
				FScopeLock ScopeLock(&StatsLock);
				++Stats.DroppedAudioFrames;
				continue;
			}
			FIPAccessUnit Unit;
			Unit.Data = Front.Data->GetData();
			Unit.Size = Front.Data->Num();
			Unit.Dts = Front.Dts;
			Unit.Pts = Front.Pts;
			Unit.Duration = Front.Duration;
			Unit.bKeyframe = true;
			const EIPDecodeResult Result = AudioDecoder->Decode(Unit);
			if (Result == EIPDecodeResult::Busy) {
				break;
			}
			EncodedAudio.RemoveAt(0, EAllowShrinking::No);
			if (Result == EIPDecodeResult::Error) {
				ReportError(FString::Printf(TEXT("audio decode failed: %s"), *AudioDecoder->GetLastError()));
				FScopeLock ScopeLock(&StatsLock);
				++Stats.DroppedAudioFrames;
			}
			PumpOutputs();
		}
	}

	void FRtmpPipeline::PumpOutputs() {
		if (VideoDecoder.IsValid()) {
			TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> Sample;
			FIntPoint Dim;
			bool bAdded = false;
			while (VideoDecoder->GetOutput(Sample, Dim)) {
				PendingVideo.Add(FDecodedVideo{Sample, Sample->GetTime().Time, Sample->GetDuration(), Dim});
				Sample.Reset();
				bAdded = true;
			}
			if (bAdded) {
				PendingVideo.StableSort([](const FDecodedVideo& A, const FDecodedVideo& B) { return A.Pts < B.Pts; });
			}
		}
		if (AudioDecoder.IsValid()) {
			TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe> Sample;
			while (AudioDecoder->GetOutput(Sample)) {
				PendingAudio.Add(FDecodedAudio{Sample, Sample->GetTime().Time, Sample->GetDuration()});
				Sample.Reset();
			}
			while (PendingAudio.Num() > MaxPendingAudio) {
				PendingAudio.RemoveAt(0);
				FScopeLock ScopeLock(&StatsLock);
				++Stats.DroppedAudioFrames;
			}
		}
	}

	void FRtmpPipeline::Release(double Now) {
		if (!Clock.IsPlaying() || bPaused.load()) {
			return;
		}
		const bool bAudioMaster = bHasAudioTrack;
		int64 DroppedAudio = 0, DroppedVideo = 0;
		while (PendingAudio.Num() > 0) {
			const FDecodedAudio& Front = PendingAudio[0];
			if (Clock.IsLate(Now, Front.Pts, Front.Duration)) {
				++DroppedAudio;
			} else if (Clock.CanRelease(Now, Front.Pts)) {
				// FMediaSamples does not enforce its limit on Enqueue: without a sound component consuming audio the
				// queue would grow forever, so drop instead (bounded memory, latency stays at the target).
				if (Samples->CanReceiveAudioSamples(1)) {
					Samples->AddAudio(Front.Sample.ToSharedRef());
				} else {
					++DroppedAudio;
				}
				if (bAudioMaster) {
					Clock.NoteReleased(Front.Pts + Front.Duration);
				}
			} else {
				break;
			}
			PendingAudio.RemoveAt(0, EAllowShrinking::No);
		}
		while (PendingVideo.Num() > 0) {
			const FDecodedVideo& Front = PendingVideo[0];
			if (Clock.IsLate(Now, Front.Pts, Front.Duration)) {
				++DroppedVideo;
			} else if (Clock.CanRelease(Now, Front.Pts) && Samples->CanReceiveVideoSamples(1)) {
				Samples->AddVideo(Front.Sample.ToSharedRef());
				if (!bAudioMaster) {
					Clock.NoteReleased(Front.Pts + Front.Duration);
				}
				FScopeLock ScopeLock(&StatsLock);
				++FramesInWindow;
				++FrameCounter;
				LastFrame.Width = Front.Dim.X;
				LastFrame.Height = Front.Dim.Y;
				LastFrame.Time = Front.Pts.GetTotalSeconds();
				Stats.Width = Front.Dim.X;
				Stats.Height = Front.Dim.Y;
			} else {
				break;
			}
			PendingVideo.RemoveAt(0, EAllowShrinking::No);
		}
		if (DroppedAudio || DroppedVideo) {
			FScopeLock ScopeLock(&StatsLock);
			Stats.DroppedAudioFrames += DroppedAudio;
			Stats.DroppedVideoFrames += DroppedVideo;
		}
	}

	void FRtmpPipeline::UpdateStats(double Now) {
		if (StatsWindowStart == 0.0) {
			StatsWindowStart = Now;
			BytesAtWindowStart = ReceivedBytes.load();
			return;
		}
		const double Elapsed = Now - StatsWindowStart;
		FScopeLock ScopeLock(&StatsLock);
		if (Elapsed >= 1.0) {
			const int64 Bytes = ReceivedBytes.load();
			Stats.Fps = static_cast<float>(FramesInWindow / Elapsed);
			Stats.BitrateKbps = static_cast<float>((Bytes - BytesAtWindowStart) * 8.0 / 1000.0 / Elapsed);
			LastFrame.Fps = Stats.Fps;
			FramesInWindow = 0;
			BytesAtWindowStart = Bytes;
			StatsWindowStart = Now;
		}
		Stats.BufferedMs = 0.0f;
		if (Clock.IsPlaying()) {
			FTimespan Newest = FTimespan::Zero();
			if (bHasAudioTrack) {
				Newest = EncodedAudio.Num() ? EncodedAudio.Last().Pts : (PendingAudio.Num() ? PendingAudio.Last().Pts : FTimespan::Zero());
			} else {
				Newest = EncodedVideo.Num() ? EncodedVideo.Last().Pts : (PendingVideo.Num() ? PendingVideo.Last().Pts : FTimespan::Zero());
			}
			Stats.BufferedMs = FMath::Max(0.0f, static_cast<float>((Newest - Clock.GetPosition(Now)).GetTotalMilliseconds()));
		}
		Stats.Rebuffers = Clock.GetRebufferCount();
	}

	FIPRtmpReceiverStats FRtmpPipeline::GetStats() const {
		FScopeLock ScopeLock(&StatsLock);
		return Stats;
	}

	FIPVideoFrameInfo FRtmpPipeline::GetLastFrame(uint64& OutFrameCounter) const {
		FScopeLock ScopeLock(&StatsLock);
		OutFrameCounter = FrameCounter;
		return LastFrame;
	}
}
