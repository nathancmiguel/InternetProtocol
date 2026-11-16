/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Player/IPPlayoutClock.h"

using namespace IPMedia;

namespace {
	constexpr EAutomationTestFlags MediaTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/**
	 * Simulates a live stream: a 20 ms audio block arrives every 20 ms of wall time (optionally with a stall),
	 * the clock releases what is due and the simulation records the latency (newest arrived - position).
	 */
	struct FSimulation {
		FPlayoutClock Clock;
		TArray<FTimespan> Pending;
		FTimespan NextPts = FTimespan::Zero();
		double Now = 0.0;
		int32 Released = 0;
		int32 Dropped = 0;
		double MaxLatencyMs = 0.0;

		explicit FSimulation(const FPlayoutSettings& Settings) : Clock(Settings) {}

		void Arrive(int32 Blocks) {
			for (int32 Index = 0; Index < Blocks; ++Index) {
				Pending.Add(NextPts);
				NextPts += FTimespan::FromMilliseconds(20);
			}
		}

		void Step(double Seconds) {
			Now += Seconds;
			Clock.Update(Now, Pending.Num() > 0, Pending.Num() ? Pending[0] : FTimespan(), Pending.Num() ? Pending.Last() : FTimespan());
			while (Pending.Num() > 0) {
				if (Clock.IsLate(Now, Pending[0], FTimespan::FromMilliseconds(20))) {
					Pending.RemoveAt(0);
					++Dropped;
				} else if (Clock.CanRelease(Now, Pending[0])) {
					Clock.NoteReleased(Pending[0] + FTimespan::FromMilliseconds(20));
					Pending.RemoveAt(0);
					++Released;
				} else {
					break;
				}
			}
			if (Clock.IsPlaying()) {
				MaxLatencyMs = FMath::Max(MaxLatencyMs, (NextPts - Clock.GetPosition(Now)).GetTotalMilliseconds());
			}
		}

		/** Steady real-time delivery for the given duration. */
		void Run(double Seconds) {
			for (double T = 0.0; T < Seconds; T += 0.02) {
				Arrive(1);
				Step(0.02);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPPlayoutStartTest, "InternetProtocol.Media.Playout.BuffersBeforeStart", MediaTestFlags)
bool FIPPlayoutStartTest::RunTest(const FString& Parameters) {
	FSimulation Sim(FPlayoutSettings::Smooth(500));
	Sim.Run(0.4);
	TestFalse(TEXT("Not playing before 500 ms are buffered"), Sim.Clock.IsPlaying());
	TestEqual(TEXT("Nothing released while buffering"), Sim.Released, 0);
	Sim.Run(0.2);
	TestTrue(TEXT("Playing once the target is buffered"), Sim.Clock.IsPlaying());
	Sim.Run(5.0);
	// Steady state: latency stays around the target (plus one block and the lead).
	const double Latency = (Sim.NextPts - Sim.Clock.GetPosition(Sim.Now)).GetTotalMilliseconds();
	TestTrue(FString::Printf(TEXT("Steady latency ~500 ms (got %.0f)"), Latency), Latency > 400.0 && Latency < 650.0);
	TestEqual(TEXT("No drops in steady state"), Sim.Dropped, 0);
	TestEqual(TEXT("No rebuffer in steady state"), Sim.Clock.GetRebufferCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPPlayoutBurstTest, "InternetProtocol.Media.Playout.CatchUpAfterBurst", MediaTestFlags)
bool FIPPlayoutBurstTest::RunTest(const FString& Parameters) {
	for (const bool bLowLatency : {true, false}) {
		const FPlayoutSettings Settings = bLowLatency ? FPlayoutSettings::LowLatency(150) : FPlayoutSettings::Smooth(1000);
		FSimulation Sim(Settings);
		Sim.Run(3.0);
		// 2 s network stall with no data, then everything arrives at once.
		for (int32 Step = 0; Step < 100; ++Step) {
			Sim.Step(0.02);
		}
		Sim.Arrive(100);
		Sim.Run(3.0);
		const double Latency = (Sim.NextPts - Sim.Clock.GetPosition(Sim.Now)).GetTotalMilliseconds();
		const double Bound = (Settings.TargetLatency + Settings.MaxExtraLatency).GetTotalMilliseconds() + 60.0;
		TestTrue(FString::Printf(TEXT("%s: latency bounded after burst (%.0f <= %.0f)"), bLowLatency ? TEXT("LowLatency") : TEXT("Smooth"), Latency, Bound),
			Latency <= Bound);
		TestTrue(TEXT("Stall caused a rebuffer"), Sim.Clock.GetRebufferCount() >= 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPPlayoutDiscontinuityTest, "InternetProtocol.Media.Playout.TimestampJump", MediaTestFlags)
bool FIPPlayoutDiscontinuityTest::RunTest(const FString& Parameters) {
	FSimulation Sim(FPlayoutSettings::LowLatency(150));
	Sim.Run(2.0);
	// Publisher restarted its clock 30 s ahead: the receiver re-bases timestamps, but if it did not, the clock
	// must still recover instead of waiting 30 s.
	Sim.NextPts += FTimespan::FromSeconds(30);
	Sim.Run(2.0);
	const double Latency = (Sim.NextPts - Sim.Clock.GetPosition(Sim.Now)).GetTotalMilliseconds();
	TestTrue(FString::Printf(TEXT("Recovered from a forward jump (latency %.0f ms)"), Latency), Latency < 400.0);
	TestTrue(TEXT("Released media after the jump"), Sim.Released > 150);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
