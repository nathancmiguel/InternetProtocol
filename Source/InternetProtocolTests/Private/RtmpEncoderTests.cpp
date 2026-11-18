/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "IPMediaEncoders.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace {
	constexpr EAutomationTestFlags EncoderTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** NAL unit types found in an Annex-B buffer. */
	TSet<uint8> NalTypes(const TArray<uint8>& AnnexB) {
		TSet<uint8> Types;
		for (int32 Index = 0; Index + 3 < AnnexB.Num(); ++Index) {
			if (AnnexB[Index] == 0 && AnnexB[Index + 1] == 0 && AnnexB[Index + 2] == 1) {
				Types.Add(AnnexB[Index + 3] & 0x1F);
				Index += 3;
			}
		}
		return Types;
	}
}

/** Hardware H.264 through AVCodecs: needs a real RHI and an NVIDIA/AMD GPU (skipped otherwise). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPH264EncoderTest, "InternetProtocol.Media.Encode.H264", EncoderTestFlags)
bool FIPH264EncoderTest::RunTest(const FString& Parameters) {
	if (!GDynamicRHI || FCString::Stricmp(GDynamicRHI->GetName(), TEXT("Null")) == 0) {
		AddWarning(TEXT("No RHI (-nullrhi): H.264 encoder test skipped."));
		return true;
	}
	if (RHIGetInterfaceType() != ERHIInterfaceType::D3D12) {
		AddWarning(TEXT("Hardware H.264 needs the D3D12 RHI: encoder test skipped."));
		return true;
	}
	TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> Encoder = FIPMediaEncoders::CreateVideoEncoder();
	FIPVideoEncoderSettings Settings;
	Settings.Width = 320;
	Settings.Height = 240;
	Settings.FrameRate = 30;
	Settings.BitrateKbps = 800;
	Settings.KeyframeIntervalSeconds = 1;
	if (!Encoder->Open(Settings)) {
		if (!IsRHIDeviceNVIDIA() && !IsRHIDeviceAMD()) {
			AddWarning(FString::Printf(TEXT("No NVIDIA/AMD GPU: H.264 encoder test skipped (%s)."), *Encoder->GetLastError()));
			return true;
		}
		AddError(FString::Printf(TEXT("Encoder did not open: %s"), *Encoder->GetLastError()));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Encoder: %s"), *Encoder->GetName()));

	FTextureRHIRef Texture;
	ENQUEUE_RENDER_COMMAND(IPCreateEncoderTestTexture)([&Texture](FRHICommandListImmediate& RHICmdList) {
		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("IPEncoderTest"), 320, 240, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::Shared)
			.SetClearValue(FClearValueBinding::None)
			.SetInitialState(ERHIAccess::Present);
		Texture = RHICmdList.CreateTexture(Desc);
	});
	FlushRenderingCommands();

	int32 Packets = 0, Keyframes = 0;
	bool bFirstIsKeyframeWithSps = false;
	int64 LastTimestamp = -1;
	bool bMonotonic = true;
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(320 * 240 * 4);
	for (int32 Frame = 0; Frame < 60; ++Frame) {
		for (int32 Pixel = 0; Pixel < 320 * 240; ++Pixel) {
			const int32 X = Pixel % 320, Y = Pixel / 320;
			Pixels[Pixel * 4 + 0] = static_cast<uint8>(X + Frame * 4);
			Pixels[Pixel * 4 + 1] = static_cast<uint8>(Y * 2);
			Pixels[Pixel * 4 + 2] = static_cast<uint8>(Frame * 8);
			Pixels[Pixel * 4 + 3] = 255;
		}
		ENQUEUE_RENDER_COMMAND(IPFillEncoderTestTexture)([&Texture, &Pixels](FRHICommandListImmediate& RHICmdList) {
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopyDest));
			RHICmdList.UpdateTexture2D(Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 320, 240), 320 * 4, Pixels.GetData());
			RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopyDest, ERHIAccess::CopySrc));
		});
		FlushRenderingCommands();
		if (!TestTrue(TEXT("Frame encoded"), Encoder->Encode(Texture, Frame * 33, Frame == 0))) {
			AddError(Encoder->GetLastError());
			break;
		}
		FIPEncodedVideoFrame Out;
		while (Encoder->Receive(Out)) {
			if (Packets == 0) {
				const TSet<uint8> Types = NalTypes(Out.AnnexB);
				bFirstIsKeyframeWithSps = Out.bKeyframe && Types.Contains(7) && Types.Contains(8) && Types.Contains(5);
			}
			++Packets;
			Keyframes += Out.bKeyframe ? 1 : 0;
			bMonotonic &= Out.TimestampMs > LastTimestamp;
			LastTimestamp = Out.TimestampMs;
		}
	}
	Encoder->Close();
	AddInfo(FString::Printf(TEXT("%d packets, %d keyframes"), Packets, Keyframes));
	TestTrue(TEXT("Most frames produced a packet"), Packets >= 55);
	TestTrue(TEXT("First packet is an IDR with SPS/PPS"), bFirstIsKeyframeWithSps);
	TestTrue(TEXT("Keyframe interval honoured (about one per second)"), Keyframes >= 2 && Keyframes <= 4);
	TestTrue(TEXT("Timestamps increase (no B-frame reordering)"), bMonotonic);
	ENQUEUE_RENDER_COMMAND(IPReleaseEncoderTestTexture)([Texture = MoveTemp(Texture)](FRHICommandListImmediate&) mutable { Texture.SafeRelease(); });
	FlushRenderingCommands();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPAacEncoderTest, "InternetProtocol.Media.Encode.AAC", EncoderTestFlags)
bool FIPAacEncoderTest::RunTest(const FString& Parameters) {
	TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> Encoder = FIPMediaEncoders::CreateAudioEncoder();
	FIPAudioEncoderSettings Settings;
	Settings.SampleRate = 48000;
	Settings.NumChannels = 2;
	Settings.BitrateKbps = 150;
	if (!TestTrue(TEXT("AAC encoder opens"), Encoder->Open(Settings))) {
		AddError(Encoder->GetLastError());
		return false;
	}
	const TArray<uint8> Asc = Encoder->GetAudioSpecificConfig();
	TestTrue(TEXT("AudioSpecificConfig is AAC-LC 48 kHz stereo"), Asc.Num() == 2 && Asc[0] == 0x11 && Asc[1] == 0x90);

	TArray<int16> Block;
	Block.SetNumUninitialized(1024 * 2);
	int32 Frames = 0;
	int64 Sample = 0, LastTimestamp = -1;
	bool bMonotonic = true;
	for (int32 Chunk = 0; Chunk < 47; ++Chunk) {
		for (int32 Index = 0; Index < 1024; ++Index, ++Sample) {
			const int16 Value = static_cast<int16>(8000.0 * FMath::Sin(2.0 * PI * 440.0 * Sample / 48000.0));
			Block[Index * 2] = Value;
			Block[Index * 2 + 1] = Value;
		}
		if (!Encoder->Encode(Block.GetData(), 1024, Chunk * 1024 * 1000 / 48000)) {
			AddError(Encoder->GetLastError());
			break;
		}
		FIPEncodedAudioFrame Out;
		while (Encoder->Receive(Out)) {
			++Frames;
			bMonotonic &= Out.TimestampMs > LastTimestamp && Out.Data.Num() > 0;
			LastTimestamp = Out.TimestampMs;
		}
	}
	Encoder->Close();
	AddInfo(FString::Printf(TEXT("%d AAC frames from 47 input blocks"), Frames));
	TestTrue(TEXT("About one AAC frame per 1024 samples"), Frames >= 44 && Frames <= 47);
	TestTrue(TEXT("Timestamps increase"), bMonotonic);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
