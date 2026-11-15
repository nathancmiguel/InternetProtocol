/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/flv.h"
#include <algorithm>
#include <string>

namespace IPNet::Rtmp {
	namespace {
		constexpr uint8 NaluIdr = 5;
		constexpr uint8 NaluSps = 7;
		constexpr uint8 NaluPps = 8;
		constexpr uint8 NaluAud = 9;

		constexpr uint32 AacSampleRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
	}

	bool ParseVideoTag(const uint8* Data, size_t Size, FFlvVideoTag& Out) {
		if (!Data || Size < 1) {
			return false;
		}
		Out = FFlvVideoTag();
		Out.bKeyframe = (Data[0] >> 4) == 1;
		Out.CodecId = Data[0] & 0x0F;
		if (Out.CodecId != Flv::CodecH264) {
			// Other codecs are passed through untouched (the receiver reports them as unsupported).
			Out.Data = Data + 1;
			Out.Size = Size - 1;
			return true;
		}
		if (Size < 5) {
			return false;
		}
		Out.PacketType = Data[1];
		int32 Cts = (static_cast<int32>(Data[2]) << 16) | (static_cast<int32>(Data[3]) << 8) | Data[4];
		if (Cts & 0x800000) {
			Cts |= ~0xFFFFFF; // sign-extend 24-bit
		}
		Out.CompositionTime = Cts;
		Out.Data = Data + 5;
		Out.Size = Size - 5;
		return true;
	}

	bool ParseAudioTag(const uint8* Data, size_t Size, FFlvAudioTag& Out) {
		if (!Data || Size < 1) {
			return false;
		}
		Out = FFlvAudioTag();
		Out.SoundFormat = Data[0] >> 4;
		if (Out.SoundFormat != Flv::SoundFormatAac) {
			Out.Data = Data + 1;
			Out.Size = Size - 1;
			return true;
		}
		if (Size < 2) {
			return false;
		}
		Out.PacketType = Data[1];
		Out.Data = Data + 2;
		Out.Size = Size - 2;
		return true;
	}

	std::vector<uint8> BuildVideoTag(bool bKeyframe, uint8 PacketType, int32 CompositionTime, const uint8* Data, size_t Size) {
		std::vector<uint8> Out;
		Out.reserve(Size + 5);
		Out.push_back(static_cast<uint8>(((bKeyframe ? 1 : 2) << 4) | Flv::CodecH264));
		Out.push_back(PacketType);
		const uint32 Cts = static_cast<uint32>(CompositionTime) & 0xFFFFFF;
		Out.push_back(static_cast<uint8>(Cts >> 16));
		Out.push_back(static_cast<uint8>(Cts >> 8));
		Out.push_back(static_cast<uint8>(Cts));
		if (Size > 0) {
			Out.insert(Out.end(), Data, Data + Size);
		}
		return Out;
	}

	std::vector<uint8> BuildAacAudioTag(uint8 PacketType, const uint8* Data, size_t Size) {
		std::vector<uint8> Out;
		Out.reserve(Size + 2);
		Out.push_back(0xAF);
		Out.push_back(PacketType);
		if (Size > 0) {
			Out.insert(Out.end(), Data, Data + Size);
		}
		return Out;
	}

	bool ParseAvcDecoderConfig(const uint8* Data, size_t Size, FAvcConfig& Out) {
		Out = FAvcConfig();
		if (!Data || Size < 7 || Data[0] != 1) {
			return false;
		}
		Out.Profile = Data[1];
		Out.Compatibility = Data[2];
		Out.Level = Data[3];
		Out.NalLengthSize = static_cast<uint8>((Data[4] & 0x03) + 1);
		if (Out.NalLengthSize == 3) {
			return false; // 3-byte lengths are not allowed
		}
		size_t Pos = 5;
		auto ReadSets = [&](int32 Count, std::vector<std::vector<uint8>>& Into) {
			for (int32 Index = 0; Index < Count; ++Index) {
				if (Size - Pos < 2) return false;
				const size_t Length = (static_cast<size_t>(Data[Pos]) << 8) | Data[Pos + 1];
				Pos += 2;
				if (Size - Pos < Length || Length == 0) return false;
				Into.emplace_back(Data + Pos, Data + Pos + Length);
				Pos += Length;
			}
			return true;
		};
		if (!ReadSets(Data[Pos++] & 0x1F, Out.Sps) || Pos >= Size) {
			return false;
		}
		if (!ReadSets(Data[Pos++], Out.Pps)) {
			return false;
		}
		// High-profile trailing fields (chroma format, bit depth, SPS ext) are ignored.
		return !Out.Sps.empty() && !Out.Pps.empty();
	}

	std::vector<uint8> BuildAvcDecoderConfig(const FAvcConfig& Config) {
		std::vector<uint8> Out;
		const std::vector<uint8>* FirstSps = Config.Sps.empty() ? nullptr : &Config.Sps[0];
		Out.push_back(1);
		// Profile/compat/level come from the SPS when available (bytes 1..3 after the NAL header).
		Out.push_back(FirstSps && FirstSps->size() > 3 ? (*FirstSps)[1] : Config.Profile);
		Out.push_back(FirstSps && FirstSps->size() > 3 ? (*FirstSps)[2] : Config.Compatibility);
		Out.push_back(FirstSps && FirstSps->size() > 3 ? (*FirstSps)[3] : Config.Level);
		Out.push_back(static_cast<uint8>(0xFC | ((Config.NalLengthSize - 1) & 0x03)));
		Out.push_back(static_cast<uint8>(0xE0 | (Config.Sps.size() & 0x1F)));
		for (const std::vector<uint8>& Sps : Config.Sps) {
			Out.push_back(static_cast<uint8>((Sps.size() >> 8) & 0xFF));
			Out.push_back(static_cast<uint8>(Sps.size() & 0xFF));
			Out.insert(Out.end(), Sps.begin(), Sps.end());
		}
		Out.push_back(static_cast<uint8>(Config.Pps.size() & 0xFF));
		for (const std::vector<uint8>& Pps : Config.Pps) {
			Out.push_back(static_cast<uint8>((Pps.size() >> 8) & 0xFF));
			Out.push_back(static_cast<uint8>(Pps.size() & 0xFF));
			Out.insert(Out.end(), Pps.begin(), Pps.end());
		}
		return Out;
	}

	void SplitAnnexB(const uint8* Data, size_t Size, std::vector<FNaluView>& Out) {
		Out.clear();
		size_t Index = 0;
		size_t Start = std::string::npos;
		while (Index + 2 < Size) {
			if (Data[Index] == 0 && Data[Index + 1] == 0 && Data[Index + 2] == 1) {
				if (Start != std::string::npos) {
					size_t End = Index;
					while (End > Start && Data[End - 1] == 0) {
						--End; // trailing zero of a 4-byte start code
					}
					if (End > Start) {
						Out.push_back(FNaluView{Data + Start, End - Start});
					}
				}
				Index += 3;
				Start = Index;
			} else {
				++Index;
			}
		}
		if (Start != std::string::npos && Start < Size) {
			Out.push_back(FNaluView{Data + Start, Size - Start});
		}
	}

	bool SplitAvcc(const uint8* Data, size_t Size, uint8 NalLengthSize, std::vector<FNaluView>& Out) {
		Out.clear();
		if (NalLengthSize != 1 && NalLengthSize != 2 && NalLengthSize != 4) {
			return false;
		}
		size_t Pos = 0;
		while (Pos < Size) {
			if (Size - Pos < NalLengthSize) {
				return false;
			}
			size_t Length = 0;
			for (uint8 Byte = 0; Byte < NalLengthSize; ++Byte) {
				Length = (Length << 8) | Data[Pos + Byte];
			}
			Pos += NalLengthSize;
			if (Size - Pos < Length) {
				return false;
			}
			if (Length > 0) {
				Out.push_back(FNaluView{Data + Pos, Length});
			}
			Pos += Length;
		}
		return true;
	}

	bool AnnexBToAvcc(const uint8* Data, size_t Size, std::vector<uint8>& Out, FAvcConfig* OutParams) {
		std::vector<FNaluView> Nalus;
		SplitAnnexB(Data, Size, Nalus);
		Out.clear();
		Out.reserve(Size + Nalus.size() * 4);
		bool bIdr = false;
		for (const FNaluView& Nalu : Nalus) {
			const uint8 Type = Nalu.Type();
			if (Type == NaluSps || Type == NaluPps) {
				if (OutParams) {
					std::vector<std::vector<uint8>>& Target = Type == NaluSps ? OutParams->Sps : OutParams->Pps;
					std::vector<uint8> Set(Nalu.Data, Nalu.Data + Nalu.Size);
					if (std::find(Target.begin(), Target.end(), Set) == Target.end()) {
						Target.push_back(MoveTemp(Set));
					}
				}
				continue;
			}
			if (Type == NaluAud) {
				continue;
			}
			bIdr |= Type == NaluIdr;
			const uint32 Length = static_cast<uint32>(Nalu.Size);
			Out.push_back(static_cast<uint8>(Length >> 24));
			Out.push_back(static_cast<uint8>(Length >> 16));
			Out.push_back(static_cast<uint8>(Length >> 8));
			Out.push_back(static_cast<uint8>(Length));
			Out.insert(Out.end(), Nalu.Data, Nalu.Data + Nalu.Size);
		}
		return bIdr;
	}

	bool AvccToAnnexB(const uint8* Data, size_t Size, uint8 NalLengthSize, std::vector<uint8>& Out) {
		std::vector<FNaluView> Nalus;
		if (!SplitAvcc(Data, Size, NalLengthSize, Nalus)) {
			return false;
		}
		Out.clear();
		Out.reserve(Size + Nalus.size() * 4);
		for (const FNaluView& Nalu : Nalus) {
			const uint8 StartCode[] = {0, 0, 0, 1};
			Out.insert(Out.end(), StartCode, StartCode + 4);
			Out.insert(Out.end(), Nalu.Data, Nalu.Data + Nalu.Size);
		}
		return true;
	}

	std::vector<uint8> BuildAudioSpecificConfig(const FAacConfig& Config) {
		int32 FrequencyIndex = 15;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AacSampleRates); ++Index) {
			if (AacSampleRates[Index] == Config.SampleRate) {
				FrequencyIndex = Index;
				break;
			}
		}
		// Bit writer: 5 bits object type, 4 bits frequency index (+24 bits explicit rate), 4 bits channels, 3 zero bits.
		uint64 Bits = 0;
		int32 Count = 0;
		auto Put = [&](uint32 Value, int32 Width) {
			Bits = (Bits << Width) | (Value & ((1u << Width) - 1));
			Count += Width;
		};
		Put(Config.ObjectType, 5);
		Put(static_cast<uint32>(FrequencyIndex), 4);
		if (FrequencyIndex == 15) {
			Put(Config.SampleRate, 24);
		}
		Put(Config.Channels, 4);
		Put(0, 3);
		const int32 Padding = (8 - (Count % 8)) % 8;
		Bits <<= Padding;
		Count += Padding;
		std::vector<uint8> Out;
		for (int32 Shift = Count - 8; Shift >= 0; Shift -= 8) {
			Out.push_back(static_cast<uint8>((Bits >> Shift) & 0xFF));
		}
		return Out;
	}

	bool ParseAudioSpecificConfig(const uint8* Data, size_t Size, FAacConfig& Out) {
		if (!Data || Size < 2) {
			return false;
		}
		uint64 Bits = 0;
		const size_t Usable = FMath::Min<size_t>(Size, 7);
		for (size_t Index = 0; Index < Usable; ++Index) {
			Bits = (Bits << 8) | Data[Index];
		}
		int32 Remaining = static_cast<int32>(Usable * 8);
		auto Get = [&](int32 Width) -> uint32 {
			Remaining -= Width;
			return Remaining < 0 ? 0 : static_cast<uint32>((Bits >> Remaining) & ((1ull << Width) - 1));
		};
		Out.ObjectType = static_cast<uint8>(Get(5));
		if (Out.ObjectType == 31) {
			Out.ObjectType = static_cast<uint8>(32 + Get(6));
		}
		const uint32 FrequencyIndex = Get(4);
		Out.SampleRate = FrequencyIndex == 15 ? Get(24) : (FrequencyIndex < UE_ARRAY_COUNT(AacSampleRates) ? AacSampleRates[FrequencyIndex] : 0);
		Out.Channels = static_cast<uint8>(Get(4));
		return Remaining >= 0 && Out.SampleRate > 0 && Out.ObjectType != 0;
	}
}
