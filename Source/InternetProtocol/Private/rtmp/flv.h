/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include <vector>

/*
 * FLV audio/video tag bodies as carried by RTMP Audio (8) and Video (9) messages, plus H.264/AAC
 * configuration helpers (avcC, AudioSpecificConfig) and Annex-B <-> AVCC conversion.
 * Parsers return views into the caller's buffer and validate every length.
 */
namespace IPNet::Rtmp {

	namespace Flv {
		constexpr uint8 CodecH264 = 7;
		constexpr uint8 SoundFormatAac = 10;
		constexpr uint8 AvcSequenceHeader = 0;
		constexpr uint8 AvcNalu = 1;
		constexpr uint8 AvcEndOfSequence = 2;
		constexpr uint8 AacSequenceHeader = 0;
		constexpr uint8 AacRaw = 1;
	}

	struct FFlvVideoTag {
		bool bKeyframe = false;
		uint8 CodecId = 0;
		uint8 PacketType = 0;
		/** Composition time offset in ms (PTS - DTS), signed 24-bit. */
		int32 CompositionTime = 0;
		const uint8* Data = nullptr;
		size_t Size = 0;
	};

	struct FFlvAudioTag {
		uint8 SoundFormat = 0;
		uint8 PacketType = 0;
		const uint8* Data = nullptr;
		size_t Size = 0;
	};

	bool ParseVideoTag(const uint8* Data, size_t Size, FFlvVideoTag& Out);
	bool ParseAudioTag(const uint8* Data, size_t Size, FFlvAudioTag& Out);

	std::vector<uint8> BuildVideoTag(bool bKeyframe, uint8 PacketType, int32 CompositionTime, const uint8* Data, size_t Size);
	/** AAC, 44 kHz flag, 16-bit, stereo flag: header byte 0xAF as required by the FLV spec for AAC. */
	std::vector<uint8> BuildAacAudioTag(uint8 PacketType, const uint8* Data, size_t Size);

	struct FAvcConfig {
		uint8 Profile = 0;
		uint8 Compatibility = 0;
		uint8 Level = 0;
		uint8 NalLengthSize = 4;
		std::vector<std::vector<uint8>> Sps;
		std::vector<std::vector<uint8>> Pps;
	};

	bool ParseAvcDecoderConfig(const uint8* Data, size_t Size, FAvcConfig& Out);
	std::vector<uint8> BuildAvcDecoderConfig(const FAvcConfig& Config);

	struct FNaluView {
		const uint8* Data = nullptr;
		size_t Size = 0;
		uint8 Type() const { return Size > 0 ? (Data[0] & 0x1F) : 0; }
	};

	/** Splits an Annex-B byte stream (00 00 01 / 00 00 00 01 start codes) into NAL units. */
	void SplitAnnexB(const uint8* Data, size_t Size, std::vector<FNaluView>& Out);
	/** Splits a length-prefixed (AVCC) access unit; false if a length runs past the end. */
	bool SplitAvcc(const uint8* Data, size_t Size, uint8 NalLengthSize, std::vector<FNaluView>& Out);

	/**
	 * Converts an Annex-B access unit to AVCC (4-byte lengths). SPS/PPS/AUD NAL units are removed from the
	 * output; SPS/PPS are collected into OutParams when provided. Returns whether an IDR slice was present.
	 */
	bool AnnexBToAvcc(const uint8* Data, size_t Size, std::vector<uint8>& Out, FAvcConfig* OutParams = nullptr);
	bool AvccToAnnexB(const uint8* Data, size_t Size, uint8 NalLengthSize, std::vector<uint8>& Out);

	struct FAacConfig {
		/** Audio object type: 2 = AAC-LC. */
		uint8 ObjectType = 2;
		uint32 SampleRate = 48000;
		uint8 Channels = 2;
	};

	std::vector<uint8> BuildAudioSpecificConfig(const FAacConfig& Config);
	bool ParseAudioSpecificConfig(const uint8* Data, size_t Size, FAacConfig& Out);
}
