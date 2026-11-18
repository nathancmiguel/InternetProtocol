/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "EngineUtils.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPRtmpExampleActor.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UnrealClient.h"

/*
 * Smoke test of the example map: opens /InternetProtocol/Example/RTMP/RtmpExample in PIE, receives an ffmpeg test
 * pattern on the screen, publishes the example camera back to the same server, and saves a screenshot to
 * Saved/Automation/RtmpExample.png. Needs ffmpeg, a real RHI and an NVIDIA/AMD GPU (skipped otherwise).
 */
namespace {
	constexpr EAutomationTestFlags ExampleTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	AIPRtmpExampleActor* FindExample() {
		UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
		if (!World) {
			return nullptr;
		}
		for (TActorIterator<AIPRtmpExampleActor> It(World); It; ++It) {
			if (It->HasActorBegunPlay()) {
				return *It;
			}
		}
		return nullptr;
	}

	FString FindFfmpegTool() {
		int32 ReturnCode = -1;
		FString Out, Err;
		if (FPlatformProcess::ExecProcess(TEXT("where.exe"), TEXT("ffmpeg"), &ReturnCode, &Out, &Err) && ReturnCode == 0) {
			TArray<FString> Lines;
			Out.ParseIntoArrayLines(Lines);
			if (Lines.Num() > 0) {
				return Lines[0].TrimStartAndEnd();
			}
		}
		return FString();
	}

	void WaitFor(FAutomationTestBase* Test, const FString& What, TFunction<bool()> Predicate, double Timeout) {
		TSharedRef<double> Start = MakeShared<double>(-1.0);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, What, Predicate, Timeout, Start]() {
			if (*Start < 0.0) {
				*Start = FPlatformTime::Seconds();
			}
			if (Predicate()) {
				return true;
			}
			if (FPlatformTime::Seconds() - *Start > Timeout) {
				Test->AddError(FString::Printf(TEXT("Timed out waiting for: %s"), *What));
				return true;
			}
			return false;
		}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpExampleMapTest, "InternetProtocol.Example.RtmpMap", ExampleTestFlags)
bool FIPRtmpExampleMapTest::RunTest(const FString& Parameters) {
	const FString Ffmpeg = FindFfmpegTool();
	if (Ffmpeg.IsEmpty() || !GDynamicRHI || FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0 || (!IsRHIDeviceNVIDIA() && !IsRHIDeviceAMD()) || RHIGetInterfaceType() != ERHIInterfaceType::D3D12) {
		AddWarning(TEXT("Example map test needs ffmpeg, a real D3D12 RHI and an NVIDIA/AMD GPU: skipped."));
		return true;
	}
	if (!TestTrue(TEXT("Example map opens"), AutomationOpenMap(TEXT("/InternetProtocol/Example/RTMP/RtmpExample")))) {
		return false;
	}
	// In the editor AutomationOpenMap also starts PIE.
	WaitFor(this, TEXT("PIE with the example actor"), []() { return FindExample() != nullptr; }, 60.0);

	const int32 Port = 19500 + FMath::RandRange(0, 400);
	TSharedRef<FProcHandle> Process = MakeShared<FProcHandle>();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Port, Ffmpeg, Process]() {
		AIPRtmpExampleActor* Example = FindExample();
		if (!Example) {
			return true;
		}
		TestTrue(TEXT("Example starts receiving"), Example->StartReceiving(Port, TEXT("live"), TEXT("demo"), EIPRtmpLatencyMode::LowLatency, 300));
		const FString Args = FString::Printf(
			TEXT("-hide_banner -loglevel error -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 ")
			TEXT("-t 20 -c:v libx264 -preset ultrafast -g 60 -pix_fmt yuv420p -c:a aac -f flv rtmp://127.0.0.1:%d/live/demo"), Port);
		*Process = FPlatformProcess::CreateProc(*Ffmpeg, *Args, true, true, true, nullptr, 0, nullptr, nullptr);
		return true;
	}));
	WaitFor(this, TEXT("the screen receives the 1280x720 test pattern"), []() {
		AIPRtmpExampleActor* Example = FindExample();
		const FIPRtmpReceiverStats Stats = Example ? Example->GetReceiverStats() : FIPRtmpReceiverStats();
		// Smoke level only: frame-rate checks live in InternetProtocol.Media.* (decode speed depends on the machine).
		return Stats.Width == 1280 && Stats.Height == 720;
	}, 30.0);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Port]() {
		if (AIPRtmpExampleActor* Example = FindExample()) {
			TestTrue(TEXT("Example starts publishing its camera"), Example->StartPublishing(FString::Printf(TEXT("rtmp://127.0.0.1:%d/live"), Port),
				TEXT("camera"), EIPPublishVideoSource::SceneCapture, 640, 360, 30, 1500, true));
		}
		return true;
	}));
	WaitFor(this, TEXT("the example camera is published"), []() {
		AIPRtmpExampleActor* Example = FindExample();
		const FIPRtmpPublisherStats Stats = Example ? Example->GetPublisherStats() : FIPRtmpPublisherStats();
		return Stats.State == EIPRtmpPublisherState::Publishing && Stats.Fps > 0.0f;
	}, 20.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]() {
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RtmpExample.png"));
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		if (AIPRtmpExampleActor* Example = FindExample()) {
			const FIPRtmpReceiverStats Rx = Example->GetReceiverStats();
			const FIPRtmpPublisherStats Tx = Example->GetPublisherStats();
			AddInfo(FString::Printf(TEXT("Receive: %dx%d %.1f fps, %s | Publish: %.1f fps, %.0f kbit/s, %s | status '%s' / '%s'"),
				Rx.Width, Rx.Height, Rx.Fps, *Rx.VideoDecoder, Tx.Fps, Tx.SentKbps, *Tx.VideoEncoder,
				*Example->GetReceiverStatus(), *Example->GetPublisherStatus()));
		}
		AddInfo(FString::Printf(TEXT("Screenshot: %s"), *Path));
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Process]() {
		if (AIPRtmpExampleActor* Example = FindExample()) {
			Example->StopPublishing();
			Example->StopReceiving();
		}
		if (Process->IsValid()) {
			if (FPlatformProcess::IsProcRunning(*Process)) {
				FPlatformProcess::TerminateProc(*Process);
			}
			FPlatformProcess::CloseProc(*Process);
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
