/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"
#include "rtmp/rtmpserver.h"

/*
 * Playback from URTMPServer to RTMP clients, checked with ffmpeg as both publisher and viewer (a third-party
 * RTMP implementation). Skipped when ffmpeg is not in PATH.
 */
namespace {
	constexpr EAutomationTestFlags PlaybackTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const PublishKey = TEXT("pubSecretK9x2");
	const TCHAR* const ViewKey = TEXT("viewKeyQ7w3");

	FString FindFfmpegExe() {
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

	/** Captures log lines so the test can prove that no key was logged. */
	class FLogCapture : public FOutputDevice {
	public:
		FLogCapture() { GLog->AddOutputDevice(this); }
		virtual ~FLogCapture() override { GLog->RemoveOutputDevice(this); }
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override {
			FScopeLock ScopeLock(&Lock);
			Text += Message;
			Text += TEXT("\n");
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		bool Contains(const TCHAR* Needle) {
			FScopeLock ScopeLock(&Lock);
			return Text.Contains(Needle, ESearchCase::CaseSensitive);
		}

	private:
		FCriticalSection Lock;
		FString Text;
	};

	/** One ffmpeg process with its combined stdout/stderr. */
	struct FFfmpegRun {
		FProcHandle Handle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FString Output;
		int32 ReturnCode = -1;
		bool bDone = false;

		bool Start(const FString& Exe, const FString& Args) {
			FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
			Handle = FPlatformProcess::CreateProc(*Exe, *Args, true, true, true, nullptr, 0, nullptr, WritePipe, nullptr, WritePipe);
			return Handle.IsValid();
		}

		/** Returns true once the process has exited. */
		bool Poll() {
			if (bDone) {
				return true;
			}
			Output += FPlatformProcess::ReadPipe(ReadPipe);
			if (FPlatformProcess::IsProcRunning(Handle)) {
				return false;
			}
			Output += FPlatformProcess::ReadPipe(ReadPipe);
			FPlatformProcess::GetProcReturnCode(Handle, &ReturnCode);
			Finish();
			return true;
		}

		void Finish() {
			if (bDone) {
				return;
			}
			bDone = true;
			if (FPlatformProcess::IsProcRunning(Handle)) {
				FPlatformProcess::TerminateProc(Handle);
			}
			FPlatformProcess::CloseProc(Handle);
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			ReadPipe = WritePipe = nullptr;
		}

		~FFfmpegRun() { Finish(); }
	};

	FString PublishArgs(int32 Port, double Seconds) {
		return FString::Printf(
			TEXT("-hide_banner -loglevel error -re -f lavfi -i testsrc2=size=320x240:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 ")
			TEXT("-t %.0f -c:v libx264 -preset ultrafast -g 30 -pix_fmt yuv420p -c:a aac -b:a 96k -f flv rtmp://127.0.0.1:%d/live/%s"),
			Seconds, Port, PublishKey);
	}

	FString ViewArgs(int32 Port, const TCHAR* Key, double Seconds) {
		return FString::Printf(TEXT("-hide_banner -loglevel info -rw_timeout 8000000 -i rtmp://127.0.0.1:%d/live/%s -t %.0f -f null -"), Port, Key, Seconds);
	}

	URTMPServer* OpenServer(FAutomationTestBase* Test) {
		URTMPServer* Server = NewObject<URTMPServer>();
		Server->AddToRoot();
		FServerBindOptions Bind;
		Bind.Address = TEXT("127.0.0.1");
		Bind.Port = 0;
		if (!Test->TestTrue(TEXT("RTMP server opens"), Server->Open(Bind))) {
			Server->RemoveFromRoot();
			return nullptr;
		}
		return Server;
	}

	void Latent(TFunction<bool()> Step) {
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(MoveTemp(Step)));
	}
}

/** ffmpeg publishes, three ffmpeg viewers watch through the view key at the same time. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpPlaybackViewersTest, "InternetProtocol.RTMP.Playback.Viewers", PlaybackTestFlags)
bool FIPRtmpPlaybackViewersTest::RunTest(const FString& Parameters) {
	const FString Ffmpeg = FindFfmpegExe();
	if (Ffmpeg.IsEmpty()) {
		AddWarning(TEXT("ffmpeg not found in PATH: playback test skipped."));
		return true;
	}
	URTMPServer* Server = OpenServer(this);
	if (!Server) {
		return false;
	}
	TestFalse(TEXT("View key equal to the stream key is refused"), Server->SetPlaybackKey(TEXT("live"), PublishKey, PublishKey));
	TestTrue(TEXT("View key accepted"), Server->SetPlaybackKey(TEXT("live"), PublishKey, ViewKey));
	Server->SetPlaybackEnabled(true, 0);
	const int32 Port = Server->LocalEndpoint().Port;

	TSharedRef<FLogCapture> Log = MakeShared<FLogCapture>();
	TSharedRef<FFfmpegRun> Publisher = MakeShared<FFfmpegRun>();
	TestTrue(TEXT("ffmpeg publisher started"), Publisher->Start(Ffmpeg, PublishArgs(Port, 12)));
	TSharedRef<TArray<TSharedRef<FFfmpegRun>>> Viewers = MakeShared<TArray<TSharedRef<FFfmpegRun>>>();
	TSharedRef<double> Start = MakeShared<double>(0.0);
	TSharedRef<int32> MaxViewers = MakeShared<int32>(0);

	// Viewers join once the stream is live.
	Latent([this, Ffmpeg, Port, Viewers, Start]() {
		if (*Start == 0.0) {
			*Start = FPlatformTime::Seconds();
		}
		if (FPlatformTime::Seconds() - *Start < 1.5) {
			return false;
		}
		for (int32 Index = 0; Index < 3; ++Index) {
			TSharedRef<FFfmpegRun> Viewer = MakeShared<FFfmpegRun>();
			TestTrue(TEXT("ffmpeg viewer started"), Viewer->Start(Ffmpeg, ViewArgs(Port, ViewKey, 3)));
			Viewers->Add(Viewer);
		}
		*Start = FPlatformTime::Seconds();
		return true;
	});
	Latent([this, Server, Viewers, Start, MaxViewers]() {
		*MaxViewers = FMath::Max(*MaxViewers, Server->GetViewerCount(TEXT("live")));
		bool bAllDone = true;
		for (const TSharedRef<FFfmpegRun>& Viewer : *Viewers) {
			bAllDone &= Viewer->Poll();
		}
		if (!bAllDone && FPlatformTime::Seconds() - *Start > 20.0) {
			AddError(TEXT("ffmpeg viewers did not finish in time"));
			return true;
		}
		return bAllDone;
	});
	Latent([this, Server, Viewers, Publisher, Log, MaxViewers]() {
		for (int32 Index = 0; Index < Viewers->Num(); ++Index) {
			const FFfmpegRun& Viewer = *(*Viewers)[Index];
			const bool bOk = Viewer.ReturnCode == 0 && Viewer.Output.Contains(TEXT("Video: h264")) && Viewer.Output.Contains(TEXT("320x240")) &&
			                 Viewer.Output.Contains(TEXT("Audio: aac")) && Viewer.Output.Contains(TEXT("time=00:00:0"));
			if (!TestTrue(FString::Printf(TEXT("Viewer %d decoded H.264 320x240 + AAC"), Index), bOk)) {
				AddInfo(Viewer.Output.Right(1500));
			}
		}
		TestEqual(TEXT("Three simultaneous viewers"), *MaxViewers, 3);
		TestFalse(TEXT("Publishing key never logged"), Log->Contains(PublishKey));
		TestFalse(TEXT("View key never logged"), Log->Contains(ViewKey));
		Publisher->Finish();
		Server->Close();
		Server->RemoveFromRoot();
		return true;
	});
	return true;
}

/** Disabled playback, unknown view key and the viewer limit are refused (ffmpeg fails to open the stream). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpPlaybackRefusedTest, "InternetProtocol.RTMP.Playback.Refused", PlaybackTestFlags)
bool FIPRtmpPlaybackRefusedTest::RunTest(const FString& Parameters) {
	const FString Ffmpeg = FindFfmpegExe();
	if (Ffmpeg.IsEmpty()) {
		AddWarning(TEXT("ffmpeg not found in PATH: playback test skipped."));
		return true;
	}
	URTMPServer* Server = OpenServer(this);
	if (!Server) {
		return false;
	}
	Server->SetPlaybackKey(TEXT("live"), PublishKey, ViewKey);
	const int32 Port = Server->LocalEndpoint().Port;

	TSharedRef<FFfmpegRun> Publisher = MakeShared<FFfmpegRun>();
	Publisher->Start(Ffmpeg, PublishArgs(Port, 20));
	TSharedRef<FFfmpegRun> Disabled = MakeShared<FFfmpegRun>();
	TSharedRef<FFfmpegRun> Unknown = MakeShared<FFfmpegRun>();
	TSharedRef<FFfmpegRun> First = MakeShared<FFfmpegRun>();
	TSharedRef<FFfmpegRun> Second = MakeShared<FFfmpegRun>();
	TSharedRef<double> Start = MakeShared<double>(0.0);

	// 1) Playback still disabled.
	Latent([Ffmpeg, Port, Disabled, Start]() {
		if (*Start == 0.0) {
			*Start = FPlatformTime::Seconds();
		}
		if (FPlatformTime::Seconds() - *Start < 1.5) {
			return false;
		}
		Disabled->Start(Ffmpeg, ViewArgs(Port, ViewKey, 2));
		return true;
	});
	Latent([Disabled]() { return Disabled->Poll(); });
	// 2) Enabled with a one-viewer limit: an unknown key fails, the second viewer is refused.
	Latent([this, Server, Ffmpeg, Port, Disabled, Unknown, First]() {
		TestTrue(TEXT("Disabled playback refused"), Disabled->ReturnCode != 0);
		Server->SetPlaybackEnabled(true, 1);
		Unknown->Start(Ffmpeg, ViewArgs(Port, TEXT("notAKey"), 2));
		First->Start(Ffmpeg, ViewArgs(Port, ViewKey, 6));
		return true;
	});
	Latent([Unknown]() { return Unknown->Poll(); });
	Latent([this, Ffmpeg, Port, Unknown, Second, First, Start]() {
		if (!TestTrue(TEXT("Unknown view key refused"), Unknown->ReturnCode != 0)) {
			AddInfo(Unknown->Output.Right(800));
		}
		First->Poll();
		Second->Start(Ffmpeg, ViewArgs(Port, ViewKey, 2));
		return true;
	});
	Latent([Second, First]() {
		First->Poll();
		return Second->Poll();
	});
	Latent([this, First]() { return First->Poll(); });
	Latent([this, Server, Publisher, First, Second]() {
		TestTrue(TEXT("Second viewer refused by the limit"), Second->ReturnCode != 0);
		TestTrue(TEXT("First viewer played"), First->ReturnCode == 0 && First->Output.Contains(TEXT("Video: h264")));
		Publisher->Finish();
		Server->Close();
		Server->RemoveFromRoot();
		return true;
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
