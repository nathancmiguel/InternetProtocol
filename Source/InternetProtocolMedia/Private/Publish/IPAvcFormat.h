/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"

/*
 * H.264 bitstream helpers for publishing: encoders emit Annex-B (start codes), RTMP/FLV carries AVCC
 * (4-byte NAL lengths) plus an AVCDecoderConfigurationRecord (avcC) built from SPS/PPS.
 */
namespace IPMedia {

	/** Splits Annex-B into NAL units (start code removed). Views point into Data. */
	inline void SplitAnnexB(const uint8* Data, int32 Size, TArray<TArrayView<const uint8>>& Out) {
		Out.Reset();
		int32 Start = -1;
		int32 Index = 0;
		auto Emit = [&](int32 End) {
			if (Start >= 0) {
				// Trailing zero bytes belong to the next start code (4-byte form).
				while (End > Start && Data[End - 1] == 0) {
					--End;
				}
				if (End > Start) {
					Out.Add(TArrayView<const uint8>(Data + Start, End - Start));
				}
			}
		};
		while (Index + 2 < Size) {
			if (Data[Index] == 0 && Data[Index + 1] == 0 && Data[Index + 2] == 1) {
				Emit(Index);
				Index += 3;
				Start = Index;
			} else {
				++Index;
			}
		}
		if (Start >= 0) {
			Emit(Size);
		}
	}

	/**
	 * Converts an Annex-B access unit to AVCC. SPS/PPS/AUD are not copied into the payload (they travel in avcC);
	 * the last SPS/PPS seen are returned so the caller can (re)build the sequence header.
	 */
	inline bool AnnexBToAvcc(const TArray<uint8>& AnnexB, TArray<uint8>& OutAvcc, TArray<uint8>& OutSps, TArray<uint8>& OutPps) {
		TArray<TArrayView<const uint8>> Nalus;
		SplitAnnexB(AnnexB.GetData(), AnnexB.Num(), Nalus);
		OutAvcc.Reset(AnnexB.Num() + 16);
		for (const TArrayView<const uint8>& Nalu : Nalus) {
			const uint8 Type = Nalu[0] & 0x1F;
			if (Type == 7) {
				OutSps = TArray<uint8>(Nalu.GetData(), Nalu.Num());
				continue;
			}
			if (Type == 8) {
				OutPps = TArray<uint8>(Nalu.GetData(), Nalu.Num());
				continue;
			}
			if (Type == 9) {
				continue;
			}
			const uint32 Length = static_cast<uint32>(Nalu.Num());
			OutAvcc.Add(static_cast<uint8>(Length >> 24));
			OutAvcc.Add(static_cast<uint8>(Length >> 16));
			OutAvcc.Add(static_cast<uint8>(Length >> 8));
			OutAvcc.Add(static_cast<uint8>(Length));
			OutAvcc.Append(Nalu.GetData(), Nalu.Num());
		}
		return OutAvcc.Num() > 0;
	}

	/** AVCDecoderConfigurationRecord (ISO/IEC 14496-15) with one SPS and one PPS, 4-byte NAL lengths. */
	inline TArray<uint8> BuildAvcC(const TArray<uint8>& Sps, const TArray<uint8>& Pps) {
		TArray<uint8> Record;
		if (Sps.Num() < 4 || Pps.Num() < 1) {
			return Record;
		}
		Record.Add(1);          // configurationVersion
		Record.Add(Sps[1]);     // AVCProfileIndication
		Record.Add(Sps[2]);     // profile_compatibility
		Record.Add(Sps[3]);     // AVCLevelIndication
		Record.Add(0xFF);       // 6 bits reserved + lengthSizeMinusOne = 3
		Record.Add(0xE1);       // 3 bits reserved + numOfSequenceParameterSets = 1
		Record.Add(static_cast<uint8>(Sps.Num() >> 8));
		Record.Add(static_cast<uint8>(Sps.Num()));
		Record.Append(Sps);
		Record.Add(1);          // numOfPictureParameterSets
		Record.Add(static_cast<uint8>(Pps.Num() >> 8));
		Record.Add(static_cast<uint8>(Pps.Num()));
		Record.Append(Pps);
		// High profiles carry chroma format / bit depth; 4:2:0 8-bit.
		const uint8 Profile = Sps[1];
		if (Profile == 100 || Profile == 110 || Profile == 122 || Profile == 144) {
			Record.Add(0xFC | 1);  // chroma_format_idc = 1
			Record.Add(0xF8 | 0);  // bit_depth_luma_minus8
			Record.Add(0xF8 | 0);  // bit_depth_chroma_minus8
			Record.Add(0);         // numOfSequenceParameterSetExt
		}
		return Record;
	}
}
