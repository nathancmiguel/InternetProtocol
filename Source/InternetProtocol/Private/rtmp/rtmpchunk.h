/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include <vector>

/*
 * RTMP chunk stream layer (RTMP spec section 5.3).
 * FRtmpChunkReader is incremental: append whatever arrived from the socket and pull complete messages.
 * Memory is bounded by the maximum message size (16 MiB) and the number of live chunk streams; any
 * violation makes the reader fail permanently, and the session closes the connection.
 * Chunk size is not artificially capped (peers such as MediaMTX use 65536, others larger): it never
 * drives allocations by itself.
 */
namespace IPNet::Rtmp {

	enum class EMessageType : uint8 {
		SetChunkSize = 1,
		Abort = 2,
		Acknowledgement = 3,
		UserControl = 4,
		WindowAckSize = 5,
		SetPeerBandwidth = 6,
		Audio = 8,
		Video = 9,
		DataAmf3 = 15,
		CommandAmf3 = 17,
		DataAmf0 = 18,
		CommandAmf0 = 20,
		Aggregate = 22,
	};

	enum class EUserControl : uint16 {
		StreamBegin = 0,
		StreamEof = 1,
		StreamDry = 2,
		SetBufferLength = 3,
		StreamIsRecorded = 4,
		PingRequest = 6,
		PingResponse = 7,
	};

	/** Well-known chunk stream ids. */
	namespace Csid {
		constexpr uint32 Control = 2;
		constexpr uint32 Command = 3;
		constexpr uint32 Audio = 4;
		constexpr uint32 Video = 6;
		constexpr uint32 Data = 5;
	}

	struct FRtmpMessage {
		uint32 Csid = Csid::Command;
		EMessageType Type = EMessageType::CommandAmf0;
		uint32 StreamId = 0;
		/** Milliseconds, 32-bit wrapping as on the wire. */
		uint32 Timestamp = 0;
		std::vector<uint8> Payload;
	};

	struct FRtmpChunkLimits {
		uint32 MaxMessageBytes = 16u * 1024u * 1024u;
		int32 MaxChunkStreams = 64;
	};

	class FRtmpChunkReader {
	public:
		enum class EResult : uint8 { NeedMore, Message, Error };

		explicit FRtmpChunkReader(const FRtmpChunkLimits& InLimits = FRtmpChunkLimits());

		void Append(const uint8* Data, size_t Size);

		/**
		 * Extracts the next complete message. SetChunkSize and Abort are applied to the reader itself and
		 * still returned so the session can observe them.
		 */
		EResult Next(FRtmpMessage& Out);

		uint32 GetChunkSize() const { return ChunkSize; }
		const FString& GetError() const { return Error; }

	private:
		struct FStreamState {
			uint32 Timestamp = 0;
			uint32 Delta = 0;
			uint32 Length = 0;
			EMessageType Type = EMessageType::CommandAmf0;
			uint32 StreamId = 0;
			bool bExtended = false;
			bool bHasHeader = false;
			std::vector<uint8> Partial;
		};

		EResult Fail(const TCHAR* Message);
		FStreamState* FindOrAddStream(uint32 Csid);

		FRtmpChunkLimits Limits;
		std::vector<uint8> Buffer;
		size_t Pos = 0;
		uint32 ChunkSize = 128;
		TMap<uint32, FStreamState> Streams;
		FString Error;
		bool bFailed = false;
	};

	class FRtmpChunkWriter {
	public:
		/** Must be called right after sending a SetChunkSize message built with MakeSetChunkSize. */
		void SetChunkSize(uint32 InChunkSize) { ChunkSize = FMath::Clamp<uint32>(InChunkSize, 1, 0x7FFFFFFF); }
		uint32 GetChunkSize() const { return ChunkSize; }

		/** Appends the chunked representation of Message to Out. */
		void Write(const FRtmpMessage& Message, std::vector<uint8>& Out) const;
		void Write(uint32 Csid, EMessageType Type, uint32 StreamId, uint32 Timestamp, const uint8* Payload, size_t Size, std::vector<uint8>& Out) const;

	private:
		uint32 ChunkSize = 128;
	};

	// Protocol control / user control message builders.
	FRtmpMessage MakeSetChunkSize(uint32 Size);
	FRtmpMessage MakeWindowAckSize(uint32 Size);
	FRtmpMessage MakeSetPeerBandwidth(uint32 Size, uint8 LimitType = 2);
	FRtmpMessage MakeAcknowledgement(uint32 SequenceNumber);
	FRtmpMessage MakeUserControl(EUserControl Event, uint32 Value);

	uint32 ReadU32BE(const uint8* Data);
	uint32 ReadU24BE(const uint8* Data);
	void PutU32BE(std::vector<uint8>& Out, uint32 Value);
	void PutU24BE(std::vector<uint8>& Out, uint32 Value);
}
