/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"

/*
 * Live playout clock / jitter buffer policy (pure logic, unit tested).
 *
 * Decoded samples carry presentation times on the player timeline. The clock decides when they may be
 * handed to the Media Framework:
 *  - Buffering: nothing is released until TargetLatency worth of media is buffered.
 *  - Playing: position advances in real time from the first buffered sample. A sample is released when
 *    Pts <= Position + Lead.
 *  - Catch-up: if more than TargetLatency + MaxExtraLatency is buffered (the network delivered a burst after
 *    a stall), the position jumps forward; samples older than the new position are dropped by the caller.
 *    This keeps latency bounded instead of growing after every hiccup.
 *  - Underrun: if the position runs past everything buffered by more than UnderrunTolerance, the clock
 *    returns to Buffering (rebuffer) so playback resumes smoothly instead of stuttering frame by frame.
 * LowLatency and Smooth only differ by their defaults (target and tolerances).
 */
namespace IPMedia {

	struct FPlayoutSettings {
		FTimespan TargetLatency = FTimespan::FromMilliseconds(1000);
		FTimespan MaxExtraLatency = FTimespan::FromMilliseconds(500);
		FTimespan UnderrunTolerance = FTimespan::FromMilliseconds(250);
		FTimespan Lead = FTimespan::FromMilliseconds(80);

		static FPlayoutSettings LowLatency(int32 TargetMs = 150) {
			FPlayoutSettings Settings;
			Settings.TargetLatency = FTimespan::FromMilliseconds(FMath::Max(TargetMs, 0));
			Settings.MaxExtraLatency = FTimespan::FromMilliseconds(120);
			Settings.UnderrunTolerance = FTimespan::FromMilliseconds(100);
			Settings.Lead = FTimespan::FromMilliseconds(40);
			return Settings;
		}

		static FPlayoutSettings Smooth(int32 TargetMs = 1000) {
			FPlayoutSettings Settings;
			Settings.TargetLatency = FTimespan::FromMilliseconds(FMath::Max(TargetMs, 0));
			Settings.MaxExtraLatency = FTimespan::FromMilliseconds(FMath::Max(500, TargetMs / 2));
			Settings.UnderrunTolerance = FTimespan::FromMilliseconds(250);
			Settings.Lead = FTimespan::FromMilliseconds(80);
			return Settings;
		}
	};

	class FPlayoutClock {
	public:
		enum class EState : uint8 { Buffering, Playing };

		explicit FPlayoutClock(const FPlayoutSettings& InSettings = FPlayoutSettings()) : Settings(InSettings) {}

		void SetSettings(const FPlayoutSettings& InSettings) { Settings = InSettings; }
		const FPlayoutSettings& GetSettings() const { return Settings; }

		void Reset() {
			State = EState::Buffering;
			StartWallSeconds = 0.0;
			StartPosition = FTimespan::Zero();
			LastReleasedEnd = FTimespan::MinValue();
			Rebuffers = 0;
			CatchUps = 0;
		}

		/**
		 * Advances the policy. Earliest/Latest are the presentation times of the oldest and newest decoded
		 * samples not yet released (ignored when bHasPending is false). Underruns are detected against the end
		 * of the last released sample (see NoteReleased).
		 */
		void Update(double NowSeconds, bool bHasPending, FTimespan Earliest, FTimespan Latest) {
			if (State == EState::Buffering) {
				if (bHasPending && Latest - Earliest >= Settings.TargetLatency) {
					State = EState::Playing;
					StartWallSeconds = NowSeconds;
					StartPosition = Earliest;
				}
				return;
			}
			const FTimespan Position = GetPosition(NowSeconds);
			if (bHasPending && Latest - Position > Settings.TargetLatency + Settings.MaxExtraLatency) {
				// Too far behind live: jump so that exactly TargetLatency stays buffered.
				StartPosition += (Latest - Position) - Settings.TargetLatency;
				++CatchUps;
				return;
			}
			const FTimespan BufferedEnd = bHasPending ? Latest : LastReleasedEnd;
			if (Position > BufferedEnd + Settings.UnderrunTolerance) {
				State = EState::Buffering;
				++Rebuffers;
			}
		}

		/** Records the end time (Pts + Duration) of a sample handed to the Media Framework. */
		void NoteReleased(FTimespan End) {
			LastReleasedEnd = FMath::Max(LastReleasedEnd, End);
		}

		bool IsPlaying() const { return State == EState::Playing; }
		EState GetState() const { return State; }

		/** Media time currently due for presentation (only meaningful while playing). */
		FTimespan GetPosition(double NowSeconds) const {
			return StartPosition + FTimespan::FromSeconds(FMath::Max(0.0, NowSeconds - StartWallSeconds));
		}

		/** Whether a sample starting at Pts may be released now. */
		bool CanRelease(double NowSeconds, FTimespan Pts) const {
			return State == EState::Playing && Pts <= GetPosition(NowSeconds) + Settings.Lead;
		}

		/** Whether a sample is already too old to be presented (dropped instead of released). */
		bool IsLate(double NowSeconds, FTimespan Pts, FTimespan Duration) const {
			return State == EState::Playing && Pts + Duration < GetPosition(NowSeconds) - Settings.Lead;
		}

		int32 GetRebufferCount() const { return Rebuffers; }
		int32 GetCatchUpCount() const { return CatchUps; }

	private:
		FPlayoutSettings Settings;
		EState State = EState::Buffering;
		double StartWallSeconds = 0.0;
		FTimespan StartPosition;
		FTimespan LastReleasedEnd = FTimespan::MinValue();
		int32 Rebuffers = 0;
		int32 CatchUps = 0;
	};
}
