/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "rtmp/rtmpchunk.h"
#include "Math/RandomStream.h"

using namespace IPNet::Rtmp;

namespace {
	constexpr EAutomationTestFlags RtmpTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FRtmpMessage MakeMessage(uint32 InCsid, EMessageType Type, uint32 StreamId, uint32 Timestamp, size_t Size, uint8 Seed) {
		FRtmpMessage Message;
		Message.Csid = InCsid;
		Message.Type = Type;
		Message.StreamId = StreamId;
		Message.Timestamp = Timestamp;
		Message.Payload.resize(Size);
		for (size_t Index = 0; Index < Size; ++Index) {
			Message.Payload[Index] = static_cast<uint8>((Index * 7 + Seed) & 0xFF);
		}
		return Message;
	}

	bool SameMessage(const FRtmpMessage& A, const FRtmpMessage& B) {
		return A.Csid == B.Csid && A.Type == B.Type && A.StreamId == B.StreamId && A.Timestamp == B.Timestamp && A.Payload == B.Payload;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpChunkRoundTripTest, "InternetProtocol.RTMP.Chunk.RoundTrip", RtmpTestFlags)
bool FIPRtmpChunkRoundTripTest::RunTest(const FString& Parameters) {
	const std::vector<FRtmpMessage> Messages = {
		MakeMessage(Csid::Command, EMessageType::CommandAmf0, 0, 0, 200, 1),
		MakeMessage(Csid::Video, EMessageType::Video, 1, 33, 150000, 2),
		MakeMessage(Csid::Audio, EMessageType::Audio, 1, 40, 400, 3),
		MakeMessage(Csid::Video, EMessageType::Video, 1, 0x00FFFFFF, 9000, 4),   // extended timestamp boundary
		MakeMessage(Csid::Video, EMessageType::Video, 1, 0xFFFFFFF0u, 5000, 5),  // near 32-bit wrap
		MakeMessage(Csid::Audio, EMessageType::Audio, 1, 41, 0, 6),             // empty payload
		MakeMessage(70, EMessageType::DataAmf0, 1, 42, 300, 7),                  // 2-byte basic header
		MakeMessage(400, EMessageType::DataAmf0, 1, 43, 300, 8),                 // 3-byte basic header
	};

	for (const uint32 ChunkSize : {128u, 4096u, 65536u, 1u << 20}) {
		FRtmpChunkWriter Writer;
		std::vector<uint8> Wire;
		// Switch the chunk size mid-stream like a real peer: first message uses 128, then SetChunkSize.
		Writer.Write(Messages[0], Wire);
		Writer.Write(MakeSetChunkSize(ChunkSize), Wire);
		Writer.SetChunkSize(ChunkSize);
		for (size_t Index = 1; Index < Messages.size(); ++Index) {
			Writer.Write(Messages[Index], Wire);
		}

		// Feed in odd-sized slices (including 1 byte) to exercise partial headers and payloads.
		for (const size_t Slice : {size_t(1), size_t(7), size_t(1000), Wire.size()}) {
			FRtmpChunkReader Reader;
			std::vector<FRtmpMessage> Received;
			for (size_t Offset = 0; Offset < Wire.size(); Offset += Slice) {
				Reader.Append(Wire.data() + Offset, FMath::Min(Slice, Wire.size() - Offset));
				FRtmpMessage Message;
				FRtmpChunkReader::EResult Result;
				while ((Result = Reader.Next(Message)) == FRtmpChunkReader::EResult::Message) {
					if (Message.Type != EMessageType::SetChunkSize) {
						Received.push_back(MoveTemp(Message));
					}
				}
				if (Result == FRtmpChunkReader::EResult::Error) {
					AddError(FString::Printf(TEXT("Reader error (chunk %u, slice %d): %s"), ChunkSize, static_cast<int32>(Slice), *Reader.GetError()));
					return false;
				}
			}
			if (!TestEqual(TEXT("Message count"), static_cast<int32>(Received.size()), static_cast<int32>(Messages.size()))) {
				return false;
			}
			for (size_t Index = 0; Index < Messages.size(); ++Index) {
				if (!SameMessage(Received[Index], Messages[Index])) {
					AddError(FString::Printf(TEXT("Message %d differs (chunk %u, slice %d)"), static_cast<int32>(Index), ChunkSize, static_cast<int32>(Slice)));
				}
			}
			TestEqual(TEXT("Reader applied SetChunkSize"), Reader.GetChunkSize(), ChunkSize);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpChunkDeltaTest, "InternetProtocol.RTMP.Chunk.CompressedHeaders", RtmpTestFlags)
bool FIPRtmpChunkDeltaTest::RunTest(const FString& Parameters) {
	// Hand-built type 0 / type 1 / type 2 / type 3 headers as sent by FFmpeg for consecutive audio packets.
	const uint8 Wire[] = {
		0x04, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x02, 0x08, 0x01, 0x00, 0x00, 0x00, 0xAA, 0xBB,  // fmt0 ts=10 len=2 audio sid=1
		0x44, 0x00, 0x00, 0x15, 0x00, 0x00, 0x01, 0x08, 0xCC,                                // fmt1 delta=21 len=1
		0x84, 0x00, 0x00, 0x15, 0xDD,                                                         // fmt2 delta=21
		0xC4, 0xEE,                                                                           // fmt3: reuse delta
	};
	FRtmpChunkReader Reader;
	Reader.Append(Wire, sizeof(Wire));
	uint32 Expected[] = {10, 31, 52, 73};
	for (const uint32 Timestamp : Expected) {
		FRtmpMessage Message;
		if (!TestEqual(TEXT("Message available"), static_cast<int32>(Reader.Next(Message)), static_cast<int32>(FRtmpChunkReader::EResult::Message))) {
			return false;
		}
		TestEqual(TEXT("Timestamp"), Message.Timestamp, Timestamp);
		TestEqual(TEXT("Stream id carried over"), Message.StreamId, 1u);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpChunkMalformedTest, "InternetProtocol.RTMP.Chunk.MalformedInput", RtmpTestFlags)
bool FIPRtmpChunkMalformedTest::RunTest(const FString& Parameters) {
	FRtmpMessage Message;
	{
		// fmt1 on a chunk stream that never had a header.
		const uint8 Wire[] = {0x45, 0, 0, 0, 0, 0, 1, 8, 0};
		FRtmpChunkReader Reader;
		Reader.Append(Wire, sizeof(Wire));
		TestEqual(TEXT("Header-less chunk rejected"), static_cast<int32>(Reader.Next(Message)), static_cast<int32>(FRtmpChunkReader::EResult::Error));
	}
	{
		// 80 KiB message against a 64 KiB limit (a 24-bit length can never exceed the default 16 MiB).
		const uint8 Wire[] = {0x06, 0, 0, 0, 0x01, 0x40, 0x00, 0x09, 1, 0, 0, 0};
		FRtmpChunkLimits Small;
		Small.MaxMessageBytes = 64 * 1024;
		FRtmpChunkReader Reader(Small);
		Reader.Append(Wire, sizeof(Wire));
		TestEqual(TEXT("Oversized message rejected"), static_cast<int32>(Reader.Next(Message)), static_cast<int32>(FRtmpChunkReader::EResult::Error));
	}
	{
		// SetChunkSize 0.
		FRtmpChunkWriter Writer;
		std::vector<uint8> Wire;
		FRtmpMessage Bad = MakeSetChunkSize(0);
		Writer.Write(Bad, Wire);
		FRtmpChunkReader Reader;
		Reader.Append(Wire.data(), Wire.size());
		TestEqual(TEXT("Chunk size 0 rejected"), static_cast<int32>(Reader.Next(Message)), static_cast<int32>(FRtmpChunkReader::EResult::Error));
	}
	{
		// Too many chunk streams.
		FRtmpChunkWriter Writer;
		std::vector<uint8> Wire;
		for (uint32 Id = 3; Id < 3 + 100; ++Id) {
			Writer.Write(MakeMessage(Id, EMessageType::Audio, 1, 0, 1, 0), Wire);
		}
		FRtmpChunkReader Reader;
		Reader.Append(Wire.data(), Wire.size());
		FRtmpChunkReader::EResult Result;
		while ((Result = Reader.Next(Message)) == FRtmpChunkReader::EResult::Message) {}
		TestEqual(TEXT("Chunk stream flood rejected"), static_cast<int32>(Result), static_cast<int32>(FRtmpChunkReader::EResult::Error));
	}

	// Garbage never crashes and memory stays bounded.
	FRandomStream Random(99);
	for (int32 Iteration = 0; Iteration < 300; ++Iteration) {
		FRtmpChunkReader Reader;
		std::vector<uint8> Garbage(static_cast<size_t>(Random.RandRange(1, 4096)));
		for (uint8& Byte : Garbage) {
			Byte = static_cast<uint8>(Random.RandRange(0, 255));
		}
		Reader.Append(Garbage.data(), Garbage.size());
		int32 Guard = 0;
		while (Reader.Next(Message) == FRtmpChunkReader::EResult::Message && ++Guard < 10000) {}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
