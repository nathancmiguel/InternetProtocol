/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPMediaEncoders.h"
#include "IPRtmpPublisher.h"
#include "IPRtmpReceiver.h"
#include "RHI.h"
#include "rtmp/rtmpserver.h"

/*
 * End-to-end publishing. Both tests need a real RHI and an NVIDIA/AMD GPU (hardware H.264); they are skipped with
 * a warning otherwise. Audio comes from EIPPublishAudioSource::Manual (a generated tone), so they also work with
 * -nosound.
 */
namespace {
	constexpr EAutomationTestFlags PublishTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	bool CanEncode(FAutomationTestBase* Test) {
		if (!GDynamicRHI || FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0) {
			Test->AddWarning(TEXT("No RHI (-nullrhi): publish test skipped."));
			return false;
		}
		if (!IsRHIDeviceNVIDIA() && !IsRHIDeviceAMD()) {
			Test->AddWarning(TEXT("No NVIDIA/AMD GPU (hardware H.264 needed): publish test skipped."));
			return false;
		}
		if (RHIGetInterfaceType() != ERHIInterfaceType::D3D12) {
			Test->AddWarning(TEXT("Publishing needs the D3D12 RHI: publish test skipped (see Publish.RequiresD3D12)."));
			return false;
		}
		return true;
	}

	UTextureRenderTarget2D* MakeTarget() {
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
		Target->AddToRoot();
		Target->RenderTargetFormat = RTF_RGBA8;
		Target->ClearColor = FLinearColor(0.1f, 0.5f, 0.9f);
		Target->InitAutoFormat(320, 240);
		Target->UpdateResourceImmediate(true);
		return Target;
	}

	/** Feeds a 440 Hz tone in real time while the latent command runs. */
	struct FTone {
		double Last = 0.0;
		int64 Sample = 0;
		void Pump(UIPRtmpPublisher* Publisher) {
			const double Now = FPlatformTime::Seconds();
			if (Last == 0.0) {
				Last = Now;
				return;
			}
			const int32 Frames = FMath::Min(static_cast<int32>((Now - Last) * 48000.0), 9600);
			if (Frames <= 0) {
				return;
			}
			Last += Frames / 48000.0;
			TArray<float> Samples;
			Samples.SetNumUninitialized(Frames * 2);
			for (int32 Index = 0; Index < Frames; ++Index, ++Sample) {
				const float Value = 0.3f * FMath::Sin(2.0 * PI * 440.0 * Sample / 48000.0);
				Samples[Index * 2] = Value;
				Samples[Index * 2 + 1] = Value;
			}
			Publisher->PushAudio(Samples.GetData(), Frames, 2, 48000);
		}
	};

	FIPRtmpPublishConfig MakeConfig(UTextureRenderTarget2D* Target) {
		FIPRtmpPublishConfig Config;
		Config.VideoSource = EIPPublishVideoSource::RenderTarget;
		Config.RenderTarget = Target;
		Config.Width = 320;
		Config.Height = 240;
		Config.FrameRate = 30;
		Config.VideoBitrateKbps = 800;
		Config.KeyframeIntervalSeconds = 1;
		Config.AudioSource = EIPPublishAudioSource::Manual;
		Config.AudioBitrateKbps = 128;
		return Config;
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

	FString FindTool(const TCHAR* Name) {
		int32 ReturnCode = -1;
		FString Out, Err;
		if (FPlatformProcess::ExecProcess(TEXT("where.exe"), Name, &ReturnCode, &Out, &Err) && ReturnCode == 0) {
			TArray<FString> Lines;
			Out.ParseIntoArrayLines(Lines);
			if (Lines.Num() > 0) {
				return Lines[0].TrimStartAndEnd();
			}
		}
		return FString();
	}
}

/** UIPRtmpPublisher -> URTMPServer -> UIPRtmpReceiver, all in this process. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpPublishLoopbackTest, "InternetProtocol.Media.Publish.Loopback", PublishTestFlags)
bool FIPRtmpPublishLoopbackTest::RunTest(const FString& Parameters) {
	if (!CanEncode(this)) {
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
	UIPRtmpReceiver* Receiver = NewObject<UIPRtmpReceiver>();
	Receiver->AddToRoot();
	TestTrue(TEXT("Receiver starts"), Receiver->StartFromServer(Server, TEXT("live"), TEXT("loop"), EIPRtmpLatencyMode::LowLatency, 300));

	UTextureRenderTarget2D* Target = MakeTarget();
	UIPRtmpPublisher* Publisher = NewObject<UIPRtmpPublisher>();
	Publisher->AddToRoot();
	const FString Url = FString::Printf(TEXT("rtmp://127.0.0.1:%d/live"), Server->LocalEndpoint().Port);
	TestTrue(TEXT("Publisher starts"), Publisher->Start(Url, TEXT("loop"), MakeConfig(Target)));

	TSharedRef<FTone> Tone = MakeShared<FTone>();
	Wait(this, TEXT("receiver decodes the published 320x240 video and 48 kHz stereo audio"), [Publisher, Receiver, Tone]() {
		Tone->Pump(Publisher);
		const FIPRtmpReceiverStats Stats = Receiver->GetStats();
		return Stats.Width == 320 && Stats.Height == 240 && Stats.Fps > 20.0f && Stats.AudioSampleRate == 48000 && Stats.AudioChannels == 2;
	}, 20.0);
	// Keep streaming a few seconds to measure steadiness.
	TSharedRef<double> Until = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Publisher, Tone, Until]() {
		if (*Until == 0.0) {
			*Until = FPlatformTime::Seconds() + 3.0;
		}
		Tone->Pump(Publisher);
		return FPlatformTime::Seconds() > *Until;
	}));
	Then([this, Publisher, Receiver]() {
		const FIPRtmpPublisherStats Pub = Publisher->GetStats();
		const FIPRtmpReceiverStats Rec = Receiver->GetStats();
		AddInfo(FString::Printf(TEXT("Publisher: %s + %s, %.1f fps, %.0f kbit/s, dropped V%lld A%lld, rtt %.1f ms"),
			*Pub.VideoEncoder, *Pub.AudioEncoder, Pub.Fps, Pub.SentKbps, Pub.DroppedVideoFrames, Pub.DroppedAudioFrames, Pub.RttMs));
		AddInfo(FString::Printf(TEXT("Receiver: %s | %s | %.1f fps | buffered %.0f ms | dropped V%lld A%lld | rebuffers %d | discontinuities %d"),
			*Rec.VideoDecoder, *Rec.AudioDecoder, Rec.Fps, Rec.BufferedMs, Rec.DroppedVideoFrames, Rec.DroppedAudioFrames, Rec.Rebuffers, Rec.Discontinuities));
		TestEqual(TEXT("Publisher is publishing"), Publisher->GetState(), EIPRtmpPublisherState::Publishing);
		TestTrue(TEXT("Publisher encodes ~30 fps"), Pub.Fps > 24.0f);
		TestTrue(TEXT("Receiver keeps up"), Rec.Fps > 20.0f);
	});
	Then([Publisher]() { Publisher->Stop(); });
	Wait(this, TEXT("receiver sees the stream end"), [Receiver]() { return !Receiver->IsStreaming(); }, 10.0);
	Then([Publisher, Receiver, Server, Target]() {
		Publisher->RemoveFromRoot();
		Receiver->Stop();
		Receiver->RemoveFromRoot();
		Server->Close();
		Server->RemoveFromRoot();
		Target->RemoveFromRoot();
	});
	return true;
}

/**
 * Third-party check: ffmpeg acts as the RTMP server (-listen 1) and must recognise H.264 320x240 and AAC 48 kHz
 * stereo. Also exercises reconnection: the publisher starts before ffmpeg listens. Skipped without ffmpeg.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpPublishFfmpegTest, "InternetProtocol.Media.Publish.ToFFmpeg", PublishTestFlags)
bool FIPRtmpPublishFfmpegTest::RunTest(const FString& Parameters) {
	if (!CanEncode(this)) {
		return true;
	}
	const FString Ffmpeg = FindTool(TEXT("ffmpeg"));
	if (Ffmpeg.IsEmpty()) {
		AddWarning(TEXT("ffmpeg not found in PATH: publish-to-ffmpeg test skipped."));
		return true;
	}
	const int32 Port = 19350 + FMath::RandRange(0, 400);
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
	const FString Args = FString::Printf(TEXT("-hide_banner -loglevel info -listen 1 -i rtmp://127.0.0.1:%d/live/key -t 3 -f null -"), Port);
	TSharedRef<FProcHandle> Process = MakeShared<FProcHandle>(FPlatformProcess::CreateProc(*Ffmpeg, *Args, true, true, true, nullptr, 0, nullptr, WritePipe, nullptr, WritePipe));
	if (!TestTrue(TEXT("ffmpeg started"), Process->IsValid())) {
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		return false;
	}

	UTextureRenderTarget2D* Target = MakeTarget();
	UIPRtmpPublisher* Publisher = NewObject<UIPRtmpPublisher>();
	Publisher->AddToRoot();
	TestTrue(TEXT("Publisher starts"), Publisher->Start(FString::Printf(TEXT("rtmp://127.0.0.1:%d/live"), Port), TEXT("key"), MakeConfig(Target)));

	TSharedRef<FString> Output = MakeShared<FString>();
	TSharedRef<FTone> Tone = MakeShared<FTone>();
	Wait(this, TEXT("ffmpeg received and decoded 3 s of the stream"), [Publisher, Tone, Process, Output, ReadPipe]() {
		Tone->Pump(Publisher);
		*Output += FPlatformProcess::ReadPipe(ReadPipe);
		return !FPlatformProcess::IsProcRunning(*Process);
	}, 30.0);
	Then([this, Publisher, Process, Output, ReadPipe, WritePipe, Target]() {
		*Output += FPlatformProcess::ReadPipe(ReadPipe);
		int32 ReturnCode = -1;
		if (FPlatformProcess::IsProcRunning(*Process)) {
			FPlatformProcess::TerminateProc(*Process);
		} else {
			FPlatformProcess::GetProcReturnCode(*Process, &ReturnCode);
		}
		FPlatformProcess::CloseProc(*Process);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		TArray<FString> Lines;
		Output->ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines) {
			if (Line.Contains(TEXT("Stream #")) || Line.Contains(TEXT("frame=")) || Line.Contains(TEXT("rror"))) {
				AddInfo(FString::Printf(TEXT("ffmpeg: %s"), *Line.Left(200)));
			}
		}
		TestEqual(TEXT("ffmpeg exit code"), ReturnCode, 0);
		TestTrue(TEXT("ffmpeg sees H.264 320x240"), Output->Contains(TEXT("Video: h264")) && Output->Contains(TEXT("320x240")));
		TestTrue(TEXT("ffmpeg sees AAC 48 kHz stereo"), Output->Contains(TEXT("Audio: aac")) && Output->Contains(TEXT("48000 Hz")) && Output->Contains(TEXT("stereo")));
		const FIPRtmpPublisherStats Stats = Publisher->GetStats();
		AddInfo(FString::Printf(TEXT("Publisher reconnect attempts: %d"), Stats.ReconnectAttempts));
		Publisher->Stop();
		Publisher->RemoveFromRoot();
		Target->RemoveFromRoot();
	});
	return true;
}

/**
 * Outside D3D12 the default (AVCodecs) encoder must refuse cleanly: on D3D11 a failed AVCodecs encoder creation
 * removes the GPU device (fatal). Run with -d3d11 to exercise it; skipped on D3D12.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpPublishRequiresD3D12Test, "InternetProtocol.Media.Publish.RequiresD3D12", PublishTestFlags)
bool FIPRtmpPublishRequiresD3D12Test::RunTest(const FString& Parameters) {
	if (!GDynamicRHI || FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0) {
		AddWarning(TEXT("No RHI: D3D12 requirement test skipped."));
		return true;
	}
	if (RHIGetInterfaceType() == ERHIInterfaceType::D3D12) {
		AddWarning(TEXT("Running on D3D12: run with -d3d11 to check the refusal."));
		return true;
	}
	TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> Encoder = FIPMediaEncoders::CreateVideoEncoder();
	TestFalse(TEXT("Default encoder refuses to open outside D3D12"), Encoder->Open(FIPVideoEncoderSettings()));
	TestTrue(TEXT("Refusal explains the D3D12 requirement"), Encoder->GetLastError().Contains(TEXT("D3D12")));

	UTextureRenderTarget2D* Target = MakeTarget();
	UIPRtmpPublisher* Publisher = NewObject<UIPRtmpPublisher>();
	Publisher->AddToRoot();
	TestFalse(TEXT("Publisher refuses to start"), Publisher->Start(TEXT("rtmp://127.0.0.1:1/live"), TEXT("key"), MakeConfig(Target)));
	TestEqual(TEXT("Publisher state is Error"), Publisher->GetState(), EIPRtmpPublisherState::Error);
	Publisher->RemoveFromRoot();
	Target->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
