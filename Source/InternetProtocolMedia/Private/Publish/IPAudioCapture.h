/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "ISubmixBufferListener.h"
#include <atomic>

/*
 * Audio side of publishing.
 *
 * FAudioRing: single-producer / single-consumer lock-free ring of interleaved stereo float frames at the output
 * rate (48 kHz), preallocated (1 s). The producer converts on the fly: any channel count -> stereo, any rate ->
 * 48 kHz (linear interpolation; the engine mixer normally runs at 48 kHz already). Overrun drops the incoming
 * frames (counted); underrun is handled by the reader (silence), so the encoder never blocks.
 *
 * FSubmixCapture: ISubmixBufferListener that feeds the ring from the audio render thread. It only copies.
 */
namespace IPMedia {

	class FAudioRing {
	public:
		static constexpr int32 OutputRate = 48000;
		static constexpr int32 OutputChannels = 2;

		explicit FAudioRing(int32 CapacityFrames = OutputRate);

		/** Producer thread. Returns the number of frames that did not fit (dropped). */
		int32 Write(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate);
		/** Consumer thread. Reads exactly NumFrames as 16-bit PCM if that many are available. */
		bool Read(int16* OutInterleaved, int32 NumFrames);
		int32 Available() const;
		int64 GetDroppedFrames() const { return DroppedFrames.load(); }
		/** Producer thread: time (FPlatformTime::Seconds) of the most recent write, 0 if none yet. */
		double GetLastWriteTime() const { return LastWriteTime.load(); }

	private:
		bool PushFrame(float Left, float Right);

		TArray<float> Buffer;
		const int32 Capacity;
		std::atomic<int64> WritePos{0};
		std::atomic<int64> ReadPos{0};
		std::atomic<int64> DroppedFrames{0};
		std::atomic<double> LastWriteTime{0.0};
		// Producer-only resampler state.
		double ResamplePhase = 0.0;
		float PrevLeft = 0.0f;
		float PrevRight = 0.0f;
		bool bHavePrev = false;
	};

	class FSubmixCapture : public ISubmixBufferListener {
	public:
		explicit FSubmixCapture(TSharedRef<FAudioRing, ESPMode::ThreadSafe> InRing) : Ring(MoveTemp(InRing)) {}

		virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels,
		                               const int32 SampleRate, double AudioClock) override;
		virtual const FString& GetListenerName() const override;

	private:
		TSharedRef<FAudioRing, ESPMode::ThreadSafe> Ring;
	};
}
