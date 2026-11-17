/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "IPRtmpReceiver.h"
#include "rtmp/rtmpserver.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace {
	constexpr EAutomationTestFlags ReceiveTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString FindFfmpeg() {
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

	void Wait(FAutomationTestBase* Test, const FString& What, TFunction<bool()> Predicate, double Timeout) {
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

	void Then(TFunction<void()> Step) {
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() {
			Step();
			return true;
		}));
	}
}

/**
 * End-to-end: FFmpeg publishes H.264 (with B-frames) + AAC to a URTMPServer; UIPRtmpReceiver decodes it with
 * the Electra decoders. Needs ffmpeg in PATH (skipped otherwise) and a real RHI (do not run with -nullrhi).
 * Electra hands decoded pictures to the GPU on the RHI submission thread, so decode throughput follows the engine
 * frame rate: run the editor unthrottled when it is in the background, e.g.
 * -ini:EditorPerProjectUserSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpReceiveFfmpegTest, "InternetProtocol.Media.ReceiveFromFFmpeg", ReceiveTestFlags)
bool FIPRtmpReceiveFfmpegTest::RunTest(const FString& Parameters) {
	const FString Ffmpeg = FindFfmpeg();
	if (Ffmpeg.IsEmpty()) {
		AddWarning(TEXT("ffmpeg not found in PATH: end-to-end receive test skipped."));
		return true;
	}

	URTMPServer* Server = NewObject<URTMPServer>();
	Server->AddToRoot();
	FServerBindOptions Bind;
	Bind.Address = TEXT("127.0.0.1");
	Bind.Port = 0;
	if (!TestTrue(TEXT("RTMP server opens"), Server->Open(Bind))) {
		Server->RemoveFromRoot();
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;

	UIPRtmpReceiver* Receiver = NewObject<UIPRtmpReceiver>();
	Receiver->AddToRoot();
	if (!TestTrue(TEXT("Receiver starts"), Receiver->StartFromServer(Server, TEXT("live"), TEXT("e2e"), EIPRtmpLatencyMode::LowLatency, 300))) {
		return false;
	}
	TestNotNull(TEXT("Video texture available"), Receiver->GetVideoTexture());

	const FString Args = FString::Printf(
		TEXT("-hide_banner -loglevel error -re -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 ")
		TEXT("-t 8 -c:v libx264 -preset ultrafast -g 30 -bf 2 -pix_fmt yuv420p -c:a aac -b:a 128k -f flv rtmp://127.0.0.1:%d/live/e2e"), Port);
	TSharedRef<FProcHandle> Process = MakeShared<FProcHandle>(FPlatformProcess::CreateProc(*Ffmpeg, *Args, true, true, true, nullptr, 0, nullptr, nullptr));
	if (!TestTrue(TEXT("ffmpeg started"), Process->IsValid())) {
		return false;
	}

	Wait(this, TEXT("decoded 640x360 video at ~30 fps with 48 kHz audio"), [Receiver]() {
		const FIPRtmpReceiverStats Stats = Receiver->GetStats();
		return Stats.Width == 640 && Stats.Height == 360 && Stats.Fps > 20.0f && Stats.AudioSampleRate == 48000;
	}, 20.0);
	Then([this, Receiver]() {
		const FIPRtmpReceiverStats Stats = Receiver->GetStats();
		AddInfo(FString::Printf(TEXT("Video decoder: %s | Audio decoder: %s | %.1f fps | %.0f kbit/s | buffered %.0f ms | dropped V%lld A%lld"),
			*Stats.VideoDecoder, *Stats.AudioDecoder, Stats.Fps, Stats.BitrateKbps, Stats.BufferedMs, Stats.DroppedVideoFrames, Stats.DroppedAudioFrames));
		TestTrue(TEXT("Receiver reports streaming"), Receiver->IsStreaming());
		TestEqual(TEXT("Audio channels"), Stats.AudioChannels, 1);
		TestTrue(TEXT("Few dropped video frames"), Stats.DroppedVideoFrames < 30);
	});
	Wait(this, TEXT("stream end after ffmpeg exits"), [Receiver]() { return !Receiver->IsStreaming(); }, 20.0);
	Then([Receiver, Server, Process]() {
		if (FPlatformProcess::IsProcRunning(*Process)) {
			FPlatformProcess::TerminateProc(*Process);
		}
		FPlatformProcess::CloseProc(*Process);
		Receiver->Stop();
		Receiver->RemoveFromRoot();
		Server->Close();
		Server->RemoveFromRoot();
	});
	return true;
}

/**
 * Pull: FFmpeg publishes to a URTMPServer with playback enabled; UIPRtmpReceiver plays it back over rtmp://
 * with a view key (IRtmpPlaySession), exactly as it would from MediaMTX or nginx-rtmp.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpReceivePullTest, "InternetProtocol.Media.ReceivePull", ReceiveTestFlags)
bool FIPRtmpReceivePullTest::RunTest(const FString& Parameters) {
	const FString Ffmpeg = FindFfmpeg();
	if (Ffmpeg.IsEmpty()) {
		AddWarning(TEXT("ffmpeg not found in PATH: pull receive test skipped."));
		return true;
	}
	URTMPServer* Server = NewObject<URTMPServer>();
	Server->AddToRoot();
	Server->SetPlaybackEnabled(true, 0);
	Server->SetPlaybackKey(TEXT("live"), TEXT("pubkey"), TEXT("viewkey"));
	FServerBindOptions Bind;
	Bind.Address = TEXT("127.0.0.1");
	Bind.Port = 0;
	if (!TestTrue(TEXT("RTMP server opens"), Server->Open(Bind))) {
		Server->RemoveFromRoot();
		return false;
	}
	const int32 Port = Server->LocalEndpoint().Port;

	const FString Args = FString::Printf(
		TEXT("-hide_banner -loglevel error -re -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 ")
		TEXT("-t 10 -c:v libx264 -preset ultrafast -g 30 -bf 2 -pix_fmt yuv420p -c:a aac -b:a 128k -f flv rtmp://127.0.0.1:%d/live/pubkey"), Port);
	TSharedRef<FProcHandle> Process = MakeShared<FProcHandle>(FPlatformProcess::CreateProc(*Ffmpeg, *Args, true, true, true, nullptr, 0, nullptr, nullptr));
	if (!TestTrue(TEXT("ffmpeg started"), Process->IsValid())) {
		return false;
	}

	UIPRtmpReceiver* Receiver = NewObject<UIPRtmpReceiver>();
	Receiver->AddToRoot();
	TestTrue(TEXT("Receiver starts pulling"), Receiver->StartPull(FString::Printf(TEXT("rtmp://127.0.0.1:%d/live"), Port), TEXT("viewkey"),
		EIPRtmpLatencyMode::LowLatency, 300));

	Wait(this, TEXT("pulled 640x360 video and 48 kHz audio decoded"), [Receiver]() {
		const FIPRtmpReceiverStats Stats = Receiver->GetStats();
		return Stats.Width == 640 && Stats.Height == 360 && Stats.Fps > 20.0f && Stats.AudioSampleRate == 48000;
	}, 20.0);
	Then([this, Receiver, Server]() {
		const FIPRtmpReceiverStats Stats = Receiver->GetStats();
		AddInfo(FString::Printf(TEXT("Pull: %s | %s | %.1f fps | %.0f kbit/s | buffered %.0f ms | dropped V%lld A%lld"),
			*Stats.VideoDecoder, *Stats.AudioDecoder, Stats.Fps, Stats.BitrateKbps, Stats.BufferedMs, Stats.DroppedVideoFrames, Stats.DroppedAudioFrames));
		TestTrue(TEXT("Receiver reports streaming"), Receiver->IsStreaming());
		TestEqual(TEXT("Server counts the pulling receiver as a viewer"), Server->GetViewerCount(TEXT("live")), 1);
	});
	Wait(this, TEXT("stream end after ffmpeg exits"), [Receiver]() { return !Receiver->IsStreaming(); }, 25.0);
	Then([Receiver, Server, Process]() {
		if (FPlatformProcess::IsProcRunning(*Process)) {
			FPlatformProcess::TerminateProc(*Process);
		}
		FPlatformProcess::CloseProc(*Process);
		Receiver->Stop();
		Receiver->RemoveFromRoot();
		Server->Close();
		Server->RemoveFromRoot();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
