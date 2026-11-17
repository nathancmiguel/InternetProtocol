/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Publish/IPAudioCapture.h"
#include "HAL/PlatformTime.h"

namespace IPMedia {

	FAudioRing::FAudioRing(int32 CapacityFrames) : Capacity(FMath::Max(CapacityFrames, 1024)) {
		Buffer.SetNumZeroed(Capacity * OutputChannels);
	}

	bool FAudioRing::PushFrame(float Left, float Right) {
		const int64 Write = WritePos.load(std::memory_order_relaxed);
		if (Write - ReadPos.load(std::memory_order_acquire) >= Capacity) {
			return false;
		}
		const int32 Index = static_cast<int32>(Write % Capacity) * OutputChannels;
		Buffer[Index] = Left;
		Buffer[Index + 1] = Right;
		WritePos.store(Write + 1, std::memory_order_release);
		return true;
	}

	int32 FAudioRing::Write(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate) {
		if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0) {
			return 0;
		}
		LastWriteTime.store(FPlatformTime::Seconds());
		int32 Dropped = 0;
		auto Stereo = [&](int32 Frame, float& OutLeft, float& OutRight) {
			const float* Samples = Interleaved + static_cast<int64>(Frame) * NumChannels;
			OutLeft = Samples[0];
			OutRight = NumChannels > 1 ? Samples[1] : Samples[0];
		};
		if (SampleRate == OutputRate) {
			for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
				float Left, Right;
				Stereo(Frame, Left, Right);
				Dropped += PushFrame(Left, Right) ? 0 : 1;
			}
		} else {
			// Linear interpolation between consecutive input frames; the phase carries over between calls.
			const double Step = static_cast<double>(SampleRate) / OutputRate;
			for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
				float Left, Right;
				Stereo(Frame, Left, Right);
				if (!bHavePrev) {
					PrevLeft = Left;
					PrevRight = Right;
					bHavePrev = true;
				}
				while (ResamplePhase < 1.0) {
					const float Alpha = static_cast<float>(ResamplePhase);
					Dropped += PushFrame(FMath::Lerp(PrevLeft, Left, Alpha), FMath::Lerp(PrevRight, Right, Alpha)) ? 0 : 1;
					ResamplePhase += Step;
				}
				ResamplePhase -= 1.0;
				PrevLeft = Left;
				PrevRight = Right;
			}
		}
		if (Dropped > 0) {
			DroppedFrames += Dropped;
		}
		return Dropped;
	}

	int32 FAudioRing::Available() const {
		return static_cast<int32>(WritePos.load(std::memory_order_acquire) - ReadPos.load(std::memory_order_relaxed));
	}

	bool FAudioRing::Read(int16* OutInterleaved, int32 NumFrames) {
		if (Available() < NumFrames) {
			return false;
		}
		int64 Read = ReadPos.load(std::memory_order_relaxed);
		for (int32 Frame = 0; Frame < NumFrames; ++Frame, ++Read) {
			const int32 Index = static_cast<int32>(Read % Capacity) * OutputChannels;
			for (int32 Channel = 0; Channel < OutputChannels; ++Channel) {
				const float Sample = FMath::Clamp(Buffer[Index + Channel], -1.0f, 1.0f);
				OutInterleaved[Frame * OutputChannels + Channel] = static_cast<int16>(FMath::RoundToInt(Sample * 32767.0f));
			}
		}
		ReadPos.store(Read, std::memory_order_release);
		return true;
	}

	void FSubmixCapture::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels,
	                                       const int32 SampleRate, double AudioClock) {
		if (NumChannels > 0) {
			Ring->Write(AudioData, NumSamples / NumChannels, NumChannels, SampleRate);
		}
	}

	const FString& FSubmixCapture::GetListenerName() const {
		static const FString Name(TEXT("InternetProtocol RTMP publisher"));
		return Name;
	}
}
