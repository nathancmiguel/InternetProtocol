/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Publish/IPAudioCapture.h"
#include "Publish/IPAvcFormat.h"

using namespace IPMedia;

namespace {
	constexpr EAutomationTestFlags PublishUnitTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPAudioRingTest, "InternetProtocol.Media.Publish.AudioRing", PublishUnitTestFlags)
bool FIPAudioRingTest::RunTest(const FString& Parameters) {
	// Underrun: reading more than available fails without consuming anything.
	{
		FAudioRing Ring(2048);
		TArray<int16> Out;
		Out.SetNumZeroed(1024 * 2);
		TestFalse(TEXT("Empty ring cannot be read"), Ring.Read(Out.GetData(), 1024));
		TArray<float> Mono;
		Mono.Init(0.5f, 512);
		Ring.Write(Mono.GetData(), 512, 1, 48000);
		TestEqual(TEXT("Mono written as frames"), Ring.Available(), 512);
		TestFalse(TEXT("Partial block cannot be read"), Ring.Read(Out.GetData(), 1024));
		TestEqual(TEXT("Nothing consumed by a failed read"), Ring.Available(), 512);
		Ring.Write(Mono.GetData(), 512, 1, 48000);
		TestTrue(TEXT("Full block read"), Ring.Read(Out.GetData(), 1024));
		TestEqual(TEXT("Mono duplicated to left"), Out[0], static_cast<int16>(16384));
		TestEqual(TEXT("Mono duplicated to right"), Out[1], static_cast<int16>(16384));
	}
	// Overrun: capacity is never exceeded, extra frames are dropped and counted.
	{
		FAudioRing Ring(2048);
		TArray<float> Stereo;
		Stereo.Init(0.25f, 3000 * 2);
		const int32 Dropped = Ring.Write(Stereo.GetData(), 3000, 2, 48000);
		TestEqual(TEXT("Ring holds its capacity"), Ring.Available(), 2048);
		TestEqual(TEXT("Excess dropped"), Dropped, 3000 - 2048);
		TestEqual(TEXT("Drop counter"), Ring.GetDroppedFrames(), static_cast<int64>(3000 - 2048));
	}
	// Clipping and multichannel: only the first two channels are kept, values are clamped.
	{
		FAudioRing Ring(2048);
		TArray<float> Surround;
		Surround.SetNumZeroed(1024 * 6);
		for (int32 Frame = 0; Frame < 1024; ++Frame) {
			Surround[Frame * 6] = 2.0f;
			Surround[Frame * 6 + 1] = -0.5f;
			Surround[Frame * 6 + 2] = 0.9f;
		}
		Ring.Write(Surround.GetData(), 1024, 6, 48000);
		TArray<int16> Out;
		Out.SetNumZeroed(1024 * 2);
		TestTrue(TEXT("5.1 block read"), Ring.Read(Out.GetData(), 1024));
		TestEqual(TEXT("Left clamped"), Out[0], static_cast<int16>(32767));
		TestEqual(TEXT("Right is channel 1"), Out[1], static_cast<int16>(FMath::RoundToInt(-0.5f * 32767.0f)));
	}
	// Resampling: 44.1 kHz input yields 48/44.1 as many frames, also when written in odd-sized pieces.
	{
		FAudioRing Ring(48000);
		TArray<float> Input;
		Input.SetNumZeroed(441 * 2);
		for (int32 Piece = 0; Piece < 100; ++Piece) {
			Ring.Write(Input.GetData(), 441, 2, 44100);
		}
		TestTrue(TEXT("One second at 44.1 kHz becomes ~48000 frames"), FMath::Abs(Ring.Available() - 48000) <= 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPAvcFormatTest, "InternetProtocol.Media.Publish.AvcFormat", PublishUnitTestFlags)
bool FIPAvcFormatTest::RunTest(const FString& Parameters) {
	// AUD, SPS (High profile), PPS, IDR with 4- and 3-byte start codes, trailing zero.
	const TArray<uint8> AnnexB = {
		0, 0, 0, 1, 0x09, 0xF0,
		0, 0, 0, 1, 0x67, 100, 0x00, 0x1F, 0xAC, 0xD9,
		0, 0, 1, 0x68, 0xEB, 0xE3, 0xCB,
		0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0x00,
	};
	TArray<uint8> Avcc, Sps, Pps;
	TestTrue(TEXT("Converted"), AnnexBToAvcc(AnnexB, Avcc, Sps, Pps));
	TestEqual(TEXT("SPS extracted"), Sps, TArray<uint8>({0x67, 100, 0x00, 0x1F, 0xAC, 0xD9}));
	TestEqual(TEXT("PPS extracted"), Pps, TArray<uint8>({0x68, 0xEB, 0xE3, 0xCB}));
	// Only the IDR remains (AUD/SPS/PPS travel in avcC); the trailing zero belongs to no NAL.
	TestEqual(TEXT("AVCC payload"), Avcc, TArray<uint8>({0, 0, 0, 5, 0x65, 0x88, 0x84, 0x00, 0x33}));

	const TArray<uint8> AvcC = BuildAvcC(Sps, Pps);
	TestTrue(TEXT("avcC header"), AvcC.Num() > 8 && AvcC[0] == 1 && AvcC[1] == 100 && AvcC[3] == 0x1F && AvcC[4] == 0xFF && AvcC[5] == 0xE1);
	TestEqual(TEXT("avcC SPS length"), static_cast<int32>((AvcC[6] << 8) | AvcC[7]), Sps.Num());
	TestEqual(TEXT("avcC with High profile extension"), AvcC.Num(), 6 + 2 + Sps.Num() + 1 + 2 + Pps.Num() + 4);
	TestEqual(TEXT("No avcC without parameter sets"), BuildAvcC(TArray<uint8>(), Pps).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
