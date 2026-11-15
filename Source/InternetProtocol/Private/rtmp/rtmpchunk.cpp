/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmpchunk.h"

namespace IPNet::Rtmp {

	uint32 ReadU32BE(const uint8* Data) {
		return (static_cast<uint32>(Data[0]) << 24) | (static_cast<uint32>(Data[1]) << 16) |
		       (static_cast<uint32>(Data[2]) << 8) | Data[3];
	}

	uint32 ReadU24BE(const uint8* Data) {
		return (static_cast<uint32>(Data[0]) << 16) | (static_cast<uint32>(Data[1]) << 8) | Data[2];
	}

	void PutU32BE(std::vector<uint8>& Out, uint32 Value) {
		for (int32 Shift = 24; Shift >= 0; Shift -= 8) {
			Out.push_back(static_cast<uint8>((Value >> Shift) & 0xFF));
		}
	}

	void PutU24BE(std::vector<uint8>& Out, uint32 Value) {
		for (int32 Shift = 16; Shift >= 0; Shift -= 8) {
			Out.push_back(static_cast<uint8>((Value >> Shift) & 0xFF));
		}
	}

	namespace {
		constexpr uint32 ExtendedMarker = 0xFFFFFF;

		FRtmpMessage MakeControl(EMessageType Type, std::vector<uint8> Payload) {
			FRtmpMessage Message;
			Message.Csid = Csid::Control;
			Message.Type = Type;
			Message.StreamId = 0;
			Message.Timestamp = 0;
			Message.Payload = MoveTemp(Payload);
			return Message;
		}
	}

	FRtmpMessage MakeSetChunkSize(uint32 Size) {
		std::vector<uint8> Payload;
		PutU32BE(Payload, Size & 0x7FFFFFFF);
		return MakeControl(EMessageType::SetChunkSize, MoveTemp(Payload));
	}

	FRtmpMessage MakeWindowAckSize(uint32 Size) {
		std::vector<uint8> Payload;
		PutU32BE(Payload, Size);
		return MakeControl(EMessageType::WindowAckSize, MoveTemp(Payload));
	}

	FRtmpMessage MakeSetPeerBandwidth(uint32 Size, uint8 LimitType) {
		std::vector<uint8> Payload;
		PutU32BE(Payload, Size);
		Payload.push_back(LimitType);
		return MakeControl(EMessageType::SetPeerBandwidth, MoveTemp(Payload));
	}

	FRtmpMessage MakeAcknowledgement(uint32 SequenceNumber) {
		std::vector<uint8> Payload;
		PutU32BE(Payload, SequenceNumber);
		return MakeControl(EMessageType::Acknowledgement, MoveTemp(Payload));
	}

	FRtmpMessage MakeUserControl(EUserControl Event, uint32 Value) {
		std::vector<uint8> Payload;
		Payload.push_back(static_cast<uint8>((static_cast<uint16>(Event) >> 8) & 0xFF));
		Payload.push_back(static_cast<uint8>(static_cast<uint16>(Event) & 0xFF));
		PutU32BE(Payload, Value);
		return MakeControl(EMessageType::UserControl, MoveTemp(Payload));
	}

	// ---------------------------------------------------------------------------------------------------------
	// Reader

	FRtmpChunkReader::FRtmpChunkReader(const FRtmpChunkLimits& InLimits) : Limits(InLimits) {}

	void FRtmpChunkReader::Append(const uint8* Data, size_t Size) {
		if (bFailed || Size == 0) {
			return;
		}
		if (Pos > 0 && Pos == Buffer.size()) {
			Buffer.clear();
			Pos = 0;
		} else if (Pos > 64 * 1024) {
			Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Pos));
			Pos = 0;
		}
		Buffer.insert(Buffer.end(), Data, Data + Size);
	}

	FRtmpChunkReader::EResult FRtmpChunkReader::Fail(const TCHAR* Message) {
		bFailed = true;
		Error = Message;
		return EResult::Error;
	}

	FRtmpChunkReader::FStreamState* FRtmpChunkReader::FindOrAddStream(uint32 InCsid) {
		if (FStreamState* Found = Streams.Find(InCsid)) {
			return Found;
		}
		if (Streams.Num() >= Limits.MaxChunkStreams) {
			return nullptr;
		}
		return &Streams.Add(InCsid);
	}

	FRtmpChunkReader::EResult FRtmpChunkReader::Next(FRtmpMessage& Out) {
		if (bFailed) {
			return EResult::Error;
		}
		for (;;) {
			const uint8* Data = Buffer.data() + Pos;
			const size_t Available = Buffer.size() - Pos;
			size_t Cursor = 0;
			auto Has = [&](size_t Count) { return Available - Cursor >= Count; };

			// Basic header.
			if (!Has(1)) return EResult::NeedMore;
			const uint8 First = Data[Cursor++];
			const uint8 Format = First >> 6;
			uint32 ChunkStream = First & 0x3F;
			if (ChunkStream == 0) {
				if (!Has(1)) return EResult::NeedMore;
				ChunkStream = 64 + Data[Cursor++];
			} else if (ChunkStream == 1) {
				if (!Has(2)) return EResult::NeedMore;
				ChunkStream = 64 + Data[Cursor] + (static_cast<uint32>(Data[Cursor + 1]) << 8);
				Cursor += 2;
			}

			FStreamState* State = FindOrAddStream(ChunkStream);
			if (!State) {
				return Fail(TEXT("too many chunk streams"));
			}
			if (Format != 0 && !State->bHasHeader) {
				return Fail(TEXT("chunk without a previous header"));
			}

			// Message header (parsed into locals; committed only once the whole chunk is available).
			uint32 Timestamp = State->Timestamp;
			uint32 Delta = State->Delta;
			uint32 Length = State->Length;
			EMessageType Type = State->Type;
			uint32 StreamId = State->StreamId;
			bool bExtended = State->bExtended;
			uint32 TimeField = 0;
			const bool bStartsMessage = State->Partial.empty();

			if (Format <= 2) {
				if (!Has(3)) return EResult::NeedMore;
				TimeField = ReadU24BE(Data + Cursor);
				Cursor += 3;
				bExtended = TimeField == ExtendedMarker;
			}
			if (Format <= 1) {
				if (!Has(4)) return EResult::NeedMore;
				Length = ReadU24BE(Data + Cursor);
				Type = static_cast<EMessageType>(Data[Cursor + 3]);
				Cursor += 4;
			}
			if (Format == 0) {
				if (!Has(4)) return EResult::NeedMore;
				StreamId = static_cast<uint32>(Data[Cursor]) | (static_cast<uint32>(Data[Cursor + 1]) << 8) |
				           (static_cast<uint32>(Data[Cursor + 2]) << 16) | (static_cast<uint32>(Data[Cursor + 3]) << 24);
				Cursor += 4;
			}
			if (bExtended) {
				if (!Has(4)) return EResult::NeedMore;
				const uint32 Extended = ReadU32BE(Data + Cursor);
				Cursor += 4;
				if (Format <= 2) {
					TimeField = Extended;
				}
			}

			if (Format <= 1 && !bStartsMessage) {
				return Fail(TEXT("new message header while a message is incomplete"));
			}
			if (Length > Limits.MaxMessageBytes) {
				return Fail(TEXT("message too large"));
			}

			// Timestamp rules (section 5.3.1.2).
			if (Format == 0) {
				Timestamp = TimeField;
				Delta = 0;
			} else if (Format == 1 || Format == 2) {
				Delta = TimeField;
				Timestamp = State->Timestamp + Delta;
			} else if (bStartsMessage) {
				Timestamp = State->Timestamp + State->Delta;
			}

			const size_t Remaining = Length - State->Partial.size();
			const size_t PayloadBytes = FMath::Min<size_t>(Remaining, ChunkSize);
			if (!Has(PayloadBytes)) {
				return EResult::NeedMore;
			}

			// Commit.
			State->Timestamp = Timestamp;
			State->Delta = Delta;
			State->Length = Length;
			State->Type = Type;
			State->StreamId = StreamId;
			State->bExtended = bExtended;
			State->bHasHeader = true;
			if (bStartsMessage) {
				State->Partial.reserve(Length);
			}
			State->Partial.insert(State->Partial.end(), Data + Cursor, Data + Cursor + PayloadBytes);
			Pos += Cursor + PayloadBytes;

			if (State->Partial.size() < Length) {
				continue;
			}

			Out.Csid = ChunkStream;
			Out.Type = Type;
			Out.StreamId = StreamId;
			Out.Timestamp = Timestamp;
			Out.Payload = MoveTemp(State->Partial);
			State->Partial = std::vector<uint8>();

			if (Type == EMessageType::SetChunkSize) {
				if (Out.Payload.size() < 4) {
					return Fail(TEXT("invalid SetChunkSize"));
				}
				const uint32 NewSize = ReadU32BE(Out.Payload.data()) & 0x7FFFFFFF;
				if (NewSize == 0) {
					return Fail(TEXT("invalid chunk size 0"));
				}
				ChunkSize = NewSize;
			} else if (Type == EMessageType::Abort && Out.Payload.size() >= 4) {
				if (FStreamState* Aborted = Streams.Find(ReadU32BE(Out.Payload.data()))) {
					Aborted->Partial = std::vector<uint8>();
				}
			}
			return EResult::Message;
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// Writer

	void FRtmpChunkWriter::Write(const FRtmpMessage& Message, std::vector<uint8>& Out) const {
		Write(Message.Csid, Message.Type, Message.StreamId, Message.Timestamp, Message.Payload.data(), Message.Payload.size(), Out);
	}

	void FRtmpChunkWriter::Write(uint32 InCsid, EMessageType Type, uint32 StreamId, uint32 Timestamp, const uint8* Payload, size_t Size,
	                             std::vector<uint8>& Out) const {
		auto PutBasicHeader = [&Out, InCsid](uint8 Format) {
			if (InCsid >= 2 && InCsid <= 63) {
				Out.push_back(static_cast<uint8>((Format << 6) | InCsid));
			} else if (InCsid >= 64 && InCsid <= 319) {
				Out.push_back(static_cast<uint8>(Format << 6));
				Out.push_back(static_cast<uint8>(InCsid - 64));
			} else {
				const uint32 Value = FMath::Clamp<uint32>(InCsid, 64, 65599) - 64;
				Out.push_back(static_cast<uint8>((Format << 6) | 1));
				Out.push_back(static_cast<uint8>(Value & 0xFF));
				Out.push_back(static_cast<uint8>((Value >> 8) & 0xFF));
			}
		};

		const bool bExtended = Timestamp >= ExtendedMarker;
		Out.reserve(Out.size() + Size + 16 + (Size / FMath::Max<uint32>(ChunkSize, 1) + 1) * 8);

		// First chunk: full (type 0) header.
		PutBasicHeader(0);
		PutU24BE(Out, bExtended ? ExtendedMarker : Timestamp);
		PutU24BE(Out, static_cast<uint32>(Size));
		Out.push_back(static_cast<uint8>(Type));
		Out.push_back(static_cast<uint8>(StreamId & 0xFF));
		Out.push_back(static_cast<uint8>((StreamId >> 8) & 0xFF));
		Out.push_back(static_cast<uint8>((StreamId >> 16) & 0xFF));
		Out.push_back(static_cast<uint8>((StreamId >> 24) & 0xFF));
		if (bExtended) {
			PutU32BE(Out, Timestamp);
		}

		size_t Offset = 0;
		for (;;) {
			const size_t Take = FMath::Min<size_t>(Size - Offset, ChunkSize);
			if (Take > 0) {
				Out.insert(Out.end(), Payload + Offset, Payload + Offset + Take);
			}
			Offset += Take;
			if (Offset >= Size) {
				break;
			}
			// Continuation: type 3 header (repeats the extended timestamp, as librtmp/FFmpeg expect).
			PutBasicHeader(3);
			if (bExtended) {
				PutU32BE(Out, Timestamp);
			}
		}
	}
}
