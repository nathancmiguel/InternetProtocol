/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "rtmp/flv.h"
#include "rtmp/rtmphandshake.h"
#include "Math/RandomStream.h"

using namespace IPNet::Rtmp;

namespace {
	constexpr EAutomationTestFlags RtmpTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	// Real-world SPS/PPS (1280x720 High@3.1 from x264) and a fake IDR slice.
	const uint8 TestSps[] = {0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9, 0x40, 0x50, 0x05, 0xBB, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC0, 0xF1, 0x83, 0x19, 0x60};
	const uint8 TestPps[] = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};
	// Fake IDR slice. Real slices never contain 00 00 01 (emulation prevention bytes are inserted).
	const uint8 TestIdr[] = {0x65, 0x88, 0x84, 0x00, 0x33, 0xFF, 0x00, 0x00, 0x03, 0x01, 0x02};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpHandshakeTest, "InternetProtocol.RTMP.Handshake", RtmpTestFlags)
bool FIPRtmpHandshakeTest::RunTest(const FString& Parameters) {
	FRtmpHandshake Client(FRtmpHandshake::ERole::Client);
	FRtmpHandshake Server(FRtmpHandshake::ERole::Server);
	std::vector<uint8> ToServer = Client.Start();
	TestEqual(TEXT("C0+C1 size"), static_cast<int32>(ToServer.size()), static_cast<int32>(1 + HandshakeSize));

	std::vector<uint8> Reply, Leftover;
	// Server receives C0C1 in two pieces.
	TestEqual(TEXT("Server needs more"), static_cast<int32>(Server.Feed(ToServer.data(), 100, Reply, Leftover)), static_cast<int32>(FRtmpHandshake::EResult::NeedMore));
	TestEqual(TEXT("Server still waiting C2"), static_cast<int32>(Server.Feed(ToServer.data() + 100, ToServer.size() - 100, Reply, Leftover)), static_cast<int32>(FRtmpHandshake::EResult::NeedMore));
	TestEqual(TEXT("S0+S1+S2 size"), static_cast<int32>(Reply.size()), static_cast<int32>(1 + 2 * HandshakeSize));
	TestTrue(TEXT("S2 echoes C1"), std::equal(Reply.begin() + 1 + HandshakeSize, Reply.end(), ToServer.begin() + 1));

	std::vector<uint8> C2;
	TestEqual(TEXT("Client done"), static_cast<int32>(Client.Feed(Reply.data(), Reply.size(), C2, Leftover)), static_cast<int32>(FRtmpHandshake::EResult::Done));
	TestEqual(TEXT("C2 size"), static_cast<int32>(C2.size()), static_cast<int32>(HandshakeSize));

	// C2 plus the first chunk bytes in the same read: the extra bytes must come back as leftover.
	std::vector<uint8> Wire = C2;
	Wire.push_back(0x03);
	Wire.push_back(0x42);
	std::vector<uint8> Nothing;
	TestEqual(TEXT("Server done"), static_cast<int32>(Server.Feed(Wire.data(), Wire.size(), Nothing, Leftover)), static_cast<int32>(FRtmpHandshake::EResult::Done));
	TestTrue(TEXT("Leftover preserved"), Leftover.size() == 2 && Leftover[0] == 0x03 && Leftover[1] == 0x42);

	FRtmpHandshake BadServer(FRtmpHandshake::ERole::Server);
	std::vector<uint8> Bad(1 + HandshakeSize, 0);
	Bad[0] = 0x06; // RTMPE: not supported
	TestEqual(TEXT("Wrong version rejected"), static_cast<int32>(BadServer.Feed(Bad.data(), Bad.size(), Reply, Leftover)), static_cast<int32>(FRtmpHandshake::EResult::Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPFlvAvcTest, "InternetProtocol.RTMP.FLV.H264", RtmpTestFlags)
bool FIPFlvAvcTest::RunTest(const FString& Parameters) {
	// Annex-B access unit as an encoder outputs it: AUD + SPS + PPS + IDR.
	std::vector<uint8> AnnexB = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1};
	AnnexB.insert(AnnexB.end(), TestSps, TestSps + sizeof(TestSps));
	AnnexB.insert(AnnexB.end(), {0, 0, 1});
	AnnexB.insert(AnnexB.end(), TestPps, TestPps + sizeof(TestPps));
	AnnexB.insert(AnnexB.end(), {0, 0, 0, 1});
	AnnexB.insert(AnnexB.end(), TestIdr, TestIdr + sizeof(TestIdr));

	FAvcConfig Params;
	std::vector<uint8> Avcc;
	TestTrue(TEXT("IDR detected"), AnnexBToAvcc(AnnexB.data(), AnnexB.size(), Avcc, &Params));
	TestEqual(TEXT("SPS collected"), static_cast<int32>(Params.Sps.size()), 1);
	TestEqual(TEXT("PPS collected"), static_cast<int32>(Params.Pps.size()), 1);
	TestEqual(TEXT("AVCC holds only the slice"), static_cast<int32>(Avcc.size()), static_cast<int32>(4 + sizeof(TestIdr)));

	// avcC round trip.
	const std::vector<uint8> Record = BuildAvcDecoderConfig(Params);
	FAvcConfig Parsed;
	TestTrue(TEXT("avcC parses"), ParseAvcDecoderConfig(Record.data(), Record.size(), Parsed));
	TestEqual(TEXT("Profile from SPS"), static_cast<int32>(Parsed.Profile), 0x64);
	TestEqual(TEXT("Level from SPS"), static_cast<int32>(Parsed.Level), 0x1F);
	TestTrue(TEXT("SPS intact"), Parsed.Sps.size() == 1 && Parsed.Sps[0] == std::vector<uint8>(TestSps, TestSps + sizeof(TestSps)));
	TestEqual(TEXT("NAL length size"), static_cast<int32>(Parsed.NalLengthSize), 4);

	// AVCC -> Annex-B -> AVCC.
	std::vector<uint8> Back, Again;
	TestTrue(TEXT("AVCC to Annex-B"), AvccToAnnexB(Avcc.data(), Avcc.size(), 4, Back));
	AnnexBToAvcc(Back.data(), Back.size(), Again);
	TestTrue(TEXT("Stable conversion"), Again == Avcc);

	// Video tag with negative composition time (allowed by the 24-bit signed field).
	const std::vector<uint8> Tag = BuildVideoTag(true, Flv::AvcNalu, -40, Avcc.data(), Avcc.size());
	FFlvVideoTag View;
	TestTrue(TEXT("Tag parses"), ParseVideoTag(Tag.data(), Tag.size(), View));
	TestTrue(TEXT("Keyframe flag"), View.bKeyframe);
	TestEqual(TEXT("Codec H.264"), static_cast<int32>(View.CodecId), static_cast<int32>(Flv::CodecH264));
	TestEqual(TEXT("Negative CTS"), View.CompositionTime, -40);
	TestEqual(TEXT("Payload size"), static_cast<int32>(View.Size), static_cast<int32>(Avcc.size()));

	// Corrupted AVCC: length past the end.
	std::vector<FNaluView> Nalus;
	const uint8 Broken[] = {0, 0, 0, 50, 0x65, 0x00};
	TestFalse(TEXT("Broken AVCC rejected"), SplitAvcc(Broken, sizeof(Broken), 4, Nalus));
	const uint8 TruncatedRecord[] = {1, 0x64, 0, 0x1F, 0xFF, 0xE1, 0x00, 0x40, 0x67};
	TestFalse(TEXT("Truncated avcC rejected"), ParseAvcDecoderConfig(TruncatedRecord, sizeof(TruncatedRecord), Parsed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPFlvAacTest, "InternetProtocol.RTMP.FLV.AAC", RtmpTestFlags)
bool FIPFlvAacTest::RunTest(const FString& Parameters) {
	const std::vector<uint8> Asc48 = BuildAudioSpecificConfig(FAacConfig{2, 48000, 2});
	TestTrue(TEXT("AAC-LC 48k stereo = 11 90"), Asc48 == std::vector<uint8>({0x11, 0x90}));
	const std::vector<uint8> Asc44 = BuildAudioSpecificConfig(FAacConfig{2, 44100, 2});
	TestTrue(TEXT("AAC-LC 44.1k stereo = 12 10"), Asc44 == std::vector<uint8>({0x12, 0x10}));

	FAacConfig Parsed;
	TestTrue(TEXT("ASC parses"), ParseAudioSpecificConfig(Asc44.data(), Asc44.size(), Parsed));
	TestEqual(TEXT("Rate"), Parsed.SampleRate, 44100u);
	TestEqual(TEXT("Channels"), static_cast<int32>(Parsed.Channels), 2);

	const std::vector<uint8> Odd = BuildAudioSpecificConfig(FAacConfig{2, 37800, 1});
	TestTrue(TEXT("Explicit rate parses"), ParseAudioSpecificConfig(Odd.data(), Odd.size(), Parsed) && Parsed.SampleRate == 37800 && Parsed.Channels == 1);

	const uint8 Raw[] = {0x21, 0x10, 0x04};
	const std::vector<uint8> Tag = BuildAacAudioTag(Flv::AacRaw, Raw, sizeof(Raw));
	FFlvAudioTag View;
	TestTrue(TEXT("Audio tag parses"), ParseAudioTag(Tag.data(), Tag.size(), View));
	TestEqual(TEXT("AAC"), static_cast<int32>(View.SoundFormat), static_cast<int32>(Flv::SoundFormatAac));
	TestEqual(TEXT("Raw packet"), static_cast<int32>(View.PacketType), static_cast<int32>(Flv::AacRaw));
	TestEqual(TEXT("Raw size"), static_cast<int32>(View.Size), 3);

	// Garbage never crashes.
	FRandomStream Random(7);
	std::vector<uint8> Garbage;
	FAvcConfig Avc;
	std::vector<FNaluView> Nalus;
	for (int32 Iteration = 0; Iteration < 2000; ++Iteration) {
		Garbage.resize(static_cast<size_t>(Random.RandRange(0, 96)));
		for (uint8& Byte : Garbage) {
			Byte = static_cast<uint8>(Random.RandRange(0, 255));
		}
		ParseAvcDecoderConfig(Garbage.data(), Garbage.size(), Avc);
		ParseAudioSpecificConfig(Garbage.data(), Garbage.size(), Parsed);
		SplitAvcc(Garbage.data(), Garbage.size(), 4, Nalus);
		SplitAnnexB(Garbage.data(), Garbage.size(), Nalus);
		FFlvVideoTag V;
		ParseVideoTag(Garbage.data(), Garbage.size(), V);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
