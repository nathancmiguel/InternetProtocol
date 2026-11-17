/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPMediaDecoders.h"
#include "IElectraCodecFactoryModule.h"
#include "IElectraCodecFactory.h"
#include "IElectraDecoder.h"
#include "IElectraDecoderOutputVideo.h"
#include "IElectraDecoderOutputAudio.h"
#include "ElectraDecodersUtils.h"
#include "ElectraDecodersPlatformResources.h"
#include "Utils/AudioChannelMapper.h"
#include "CodecTypeFormat.h"
#include "MediaDecoderOutput.h"
#include "ParameterDictionary.h"
#include "PlayerTime.h"
#include "ElectraTextureSample.h"
#include "IElectraAudioSample.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

/*
 * Electra based decoders (Windows: Media Foundation H.264 / AAC MFTs, hardware accelerated through
 * D3D11/D3D12). Video frames stay on the GPU: FElectraTextureSample converts NV12 -> RGB in a shader when
 * the Media Framework renders the sample, so there is no GPU->CPU copy.
 */
namespace IPMedia {
	namespace {
		IElectraCodecFactoryModule* GetFactoryModule() {
			// Loaded on the Game Thread by FInternetProtocolMediaModule::StartupModule; here we only look it up.
			return static_cast<IElectraCodecFactoryModule*>(FModuleManager::Get().GetModule(TEXT("ElectraCodecFactory")));
		}

		uint32 Make4CC(uint8 A, uint8 B, uint8 C, uint8 D) {
			return (static_cast<uint32>(A) << 24) | (static_cast<uint32>(B) << 16) | (static_cast<uint32>(C) << 8) | D;
		}

		struct FPendingAccessUnit {
			FElectraDecoderInputAccessUnit DecAU;
			TArray<uint8> Copy;
			TSharedPtr<IElectraDecoderBitstreamInfo, ESPMode::ThreadSafe> Bsi;
			TMap<FString, FVariant> Options;
			bool bValid = false;
		};

		struct FInFlight {
			FTimespan Pts;
			FTimespan Duration;
		};

		/**
		 * avcC record -> SPS/PPS as an Annex-B byte stream (00 00 00 01 + NAL). The Windows H.264 decoder expects
		 * this in the "csd" option and prepends it to every IDR frame; "dcr" (the avcC) is used by the bitstream
		 * processor to convert AVCC lengths. Returns an empty array if the record is malformed.
		 */
		TArray<uint8> AvcCToAnnexBParameterSets(const TArray<uint8>& Record) {
			TArray<uint8> Out;
			if (Record.Num() < 7 || Record[0] != 1) {
				return Out;
			}
			int32 Pos = 5;
			auto CopySets = [&](int32 Count) {
				for (int32 Index = 0; Index < Count; ++Index) {
					if (Record.Num() - Pos < 2) return false;
					const int32 Length = (Record[Pos] << 8) | Record[Pos + 1];
					Pos += 2;
					if (Length <= 0 || Record.Num() - Pos < Length) return false;
					const uint8 StartCode[] = {0, 0, 0, 1};
					Out.Append(StartCode, 4);
					Out.Append(Record.GetData() + Pos, Length);
					Pos += Length;
				}
				return true;
			};
			if (!CopySets(Record[Pos++] & 0x1F) || Pos >= Record.Num() || !CopySets(Record[Pos++])) {
				Out.Reset();
			}
			return Out;
		}
	}

	// -----------------------------------------------------------------------------------------------------
	// Video

	class FElectraVideoDecoder final : public IIPVideoDecoder {
	public:
		virtual ~FElectraVideoDecoder() override { Close(); }

		virtual FString GetName() const override { return Name; }
		virtual FString GetLastError() const override { return LastError; }

		virtual bool Configure(const TArray<uint8>& AvcDecoderConfig, int32 MaxWidth, int32 MaxHeight) override {
			if (Decoder.IsValid() && AvcDecoderConfig == CurrentConfig) {
				return true;
			}
			const TArray<uint8> ParameterSets = AvcCToAnnexBParameterSets(AvcDecoderConfig);
			if (ParameterSets.Num() == 0) {
				LastError = TEXT("malformed H.264 decoder configuration record (avcC)");
				return false;
			}
			TMap<FString, FVariant> NewCsd;
			NewCsd.Add(TEXT("dcr"), FVariant(AvcDecoderConfig));
			NewCsd.Add(TEXT("csd"), FVariant(ParameterSets));
			if (Decoder.IsValid() && Decoder->IsCompatibleWith(NewCsd) == IElectraDecoder::ECSDCompatibility::Compatible) {
				CurrentConfig = AvcDecoderConfig;
				Csd = MoveTemp(NewCsd);
				return true;
			}
			// New SPS/PPS that the decoder cannot take as is (resolution/profile change): recreate it.
			Close();
			CurrentConfig = AvcDecoderConfig;
			Csd = MoveTemp(NewCsd);

			IElectraCodecFactoryModule* FactoryModule = GetFactoryModule();
			if (!FactoryModule) {
				LastError = TEXT("ElectraCodecFactory module is not loaded");
				return false;
			}
			Electra::FCodecTypeFormat Format;
			Format.Type = Electra::FCodecTypeFormat::EType::Video;
			Format.FourCC = Make4CC('a', 'v', 'c', '1');
			Format.DCR = AvcDecoderConfig;
			if (!ElectraDecodersUtil::PrepareCodecTypeFormat(Format)) {
				LastError = TEXT("invalid H.264 decoder configuration (avcC)");
				return false;
			}
			int32 Width = MaxWidth, Height = MaxHeight;
			if (Format.Properties.IsType<Electra::FCodecTypeFormat::FVideo>()) {
				const Electra::FCodecTypeFormat::FVideo& Video = Format.Properties.Get<Electra::FCodecTypeFormat::FVideo>();
				Width = FMath::Max<int32>(Width, static_cast<int32>(Video.Width));
				Height = FMath::Max<int32>(Height, static_cast<int32>(Video.Height));
			}

			TMap<FString, FVariant> Config;
			Config.Add(TEXT("max_output_buffers"), FVariant(static_cast<uint32>(20)));
			Config.Add(TEXT("max_width"), FVariant(static_cast<uint32>(FMath::Max(Width, 16))));
			Config.Add(TEXT("max_height"), FVariant(static_cast<uint32>(FMath::Max(Height, 16))));

			TMap<FString, FVariant> FormatInfo;
			TSharedPtr<IElectraCodecFactory, ESPMode::ThreadSafe> Factory = FactoryModule->GetBestDecoderFactoryForFormat(FormatInfo, Format, Config);
			if (!Factory.IsValid()) {
				LastError = FString::Printf(TEXT("no H.264 decoder available for %s"), *Format.RFC6381);
				return false;
			}
			PlatformResource = FElectraDecodersPlatformResources::CreatePlatformVideoResource(this, Config);
			Decoder = Factory->CreateDecoder(Format, Config);
			if (!Decoder.IsValid() || Decoder->GetError().IsSet()) {
				LastError = Decoder.IsValid() ? Decoder->GetError().GetMessage() : FString(TEXT("failed to create the H.264 decoder"));
				Close();
				return false;
			}
			Bitstream = Decoder->CreateBitstreamProcessor();
			Name = FString::Printf(TEXT("Electra %s (%s)"), *Format.HumanReadableFormatInfo, *Factory->GetProviderInformation().GetName());
			if (!Pool.IsValid()) {
				Pool = MakeShareable(new FElectraTextureSamplePool);
			}
			return true;
		}

		virtual EIPDecodeResult Decode(const FIPAccessUnit& AccessUnit) override {
			if (!Decoder.IsValid()) {
				LastError = TEXT("decoder is not configured");
				return EIPDecodeResult::Error;
			}
			if (!Pending.bValid) {
				// Copy: the bitstream processor converts AVCC to Annex-B in place.
				Pending.Copy = TArray<uint8>(AccessUnit.Data, AccessUnit.Size);
				Pending.DecAU = FElectraDecoderInputAccessUnit();
				Pending.DecAU.Data = Pending.Copy.GetData();
				Pending.DecAU.DataSize = Pending.Copy.Num();
				Pending.DecAU.DTS = AccessUnit.Dts;
				Pending.DecAU.PTS = AccessUnit.Pts;
				Pending.DecAU.Duration = AccessUnit.Duration;
				Pending.DecAU.UserValue = NextUserValue;
				Pending.DecAU.Flags = AccessUnit.bKeyframe ? EElectraDecoderFlags::IsSyncSample : EElectraDecoderFlags::None;
				Pending.Options = AccessUnit.bKeyframe ? Csd : TMap<FString, FVariant>();
				Pending.Bsi.Reset();
				if (Bitstream.IsValid() &&
				    Bitstream->ProcessInputForDecoding(Pending.Bsi, Pending.DecAU, Pending.Options) == IElectraDecoderBitstreamProcessor::EProcessResult::Error) {
					LastError = Bitstream->GetLastError();
					return EIPDecodeResult::Error;
				}
				Pending.bValid = true;
			}
			const IElectraDecoder::EDecoderError Result = Decoder->DecodeAccessUnit(Pending.DecAU, Pending.Options);
			if (Result == IElectraDecoder::EDecoderError::NoBuffer) {
				return EIPDecodeResult::Busy;
			}
			Pending.bValid = false;
			if (Result != IElectraDecoder::EDecoderError::None) {
				LastError = Decoder->GetError().IsSet() ? Decoder->GetError().GetMessage() : FString(TEXT("H.264 decode error"));
				return EIPDecodeResult::Error;
			}
			InFlight.Add(NextUserValue++, FInFlight{AccessUnit.Pts, AccessUnit.Duration});
			// Bound the bookkeeping if a decoder silently drops frames.
			if (InFlight.Num() > 256) {
				InFlight.Empty();
			}
			return EIPDecodeResult::Ok;
		}

		virtual bool GetOutput(TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe>& OutSample, FIntPoint& OutDimensions) override {
			if (!Decoder.IsValid()) {
				return false;
			}
			for (;;) {
				const IElectraDecoder::EOutputStatus Status = Decoder->HaveOutput();
				if (Status != IElectraDecoder::EOutputStatus::Available) {
					return false;
				}
				TSharedPtr<IElectraDecoderOutput, ESPMode::ThreadSafe> Output = Decoder->GetOutput();
				if (!Output.IsValid() || Output->GetType() != IElectraDecoderOutput::EType::Video) {
					continue;
				}
				FInFlight Input;
				if (!InFlight.RemoveAndCopyValue(Output->GetUserValue(), Input)) {
					Input.Pts = Output->GetPTS();
					Input.Duration = FTimespan::FromMilliseconds(33);
				}
				TSharedPtr<IElectraDecoderVideoOutput, ESPMode::ThreadSafe> VideoOutput = StaticCastSharedPtr<IElectraDecoderVideoOutput>(Output);
				FElectraTextureSamplePtr Sample = Pool->AcquireShared();
				TSharedPtr<Electra::FParamDict, ESPMode::ThreadSafe> Properties = MakeShared<Electra::FParamDict, ESPMode::ThreadSafe>();
				Electra::FTimeValue Pts, Duration;
				Pts.SetFromTimespan(Input.Pts, 0);
				Duration.SetFromTimespan(Input.Duration);
				Properties->Set(IDecoderOutputOptionNames::PTS, Electra::FVariantValue(Pts));
				Properties->Set(IDecoderOutputOptionNames::Duration, Electra::FVariantValue(Duration));
				FString Error;
				if (!FElectraDecodersPlatformResources::SetupOutputTextureSample(Error, Sample, VideoOutput, Properties, PlatformResource)) {
					LastError = Error;
					continue;
				}
				OutDimensions = FIntPoint(VideoOutput->GetWidth(), VideoOutput->GetHeight());
				OutSample = Sample;
				return true;
			}
		}

		virtual void Drain() override {
			if (Decoder.IsValid()) {
				Decoder->SendEndOfData();
			}
		}

		virtual void Close() override {
			if (Decoder.IsValid()) {
				Decoder->Close();
				Decoder.Reset();
			}
			Bitstream.Reset();
			if (PlatformResource) {
				FElectraDecodersPlatformResources::ReleasePlatformVideoResource(this, PlatformResource);
				PlatformResource = nullptr;
			}
			Pending = FPendingAccessUnit();
			InFlight.Empty();
		}

	private:
		TSharedPtr<IElectraDecoder, ESPMode::ThreadSafe> Decoder;
		TSharedPtr<IElectraDecoderBitstreamProcessor, ESPMode::ThreadSafe> Bitstream;
		FElectraDecodersPlatformResources::IDecoderPlatformResource* PlatformResource = nullptr;
		TSharedPtr<FElectraTextureSamplePool, ESPMode::ThreadSafe> Pool;
		TMap<FString, FVariant> Csd;
		TArray<uint8> CurrentConfig;
		FPendingAccessUnit Pending;
		TMap<uint64, FInFlight> InFlight;
		uint64 NextUserValue = 1;
		FString Name = TEXT("Electra H.264");
		FString LastError;
	};

	// -----------------------------------------------------------------------------------------------------
	// Audio

	class FElectraAudioDecoder final : public IIPAudioDecoder {
	public:
		virtual ~FElectraAudioDecoder() override { Close(); }

		virtual FString GetName() const override { return Name; }
		virtual FString GetLastError() const override { return LastError; }

		virtual bool Configure(const TArray<uint8>& AudioSpecificConfig, int32 SampleRate, int32 NumChannels) override {
			if (Decoder.IsValid() && AudioSpecificConfig == CurrentConfig) {
				return true;
			}
			Close();
			CurrentConfig = AudioSpecificConfig;
			Csd.Reset();
			Csd.Add(TEXT("csd"), FVariant(AudioSpecificConfig));

			IElectraCodecFactoryModule* FactoryModule = GetFactoryModule();
			if (!FactoryModule) {
				LastError = TEXT("ElectraCodecFactory module is not loaded");
				return false;
			}
			const int32 ObjectType = AudioSpecificConfig.Num() > 0 ? (AudioSpecificConfig[0] >> 3) : 2;
			Electra::FCodecTypeFormat Format;
			Format.Type = Electra::FCodecTypeFormat::EType::Audio;
			Format.FourCC = Make4CC('m', 'p', '4', 'a');
			Format.RFC6381 = FString::Printf(TEXT("mp4a.40.%d"), ObjectType);
			Format.CSD = AudioSpecificConfig;
			Format.Properties.Emplace<Electra::FCodecTypeFormat::FAudio>();
			Electra::FCodecTypeFormat::FAudio& Audio = Format.Properties.Get<Electra::FCodecTypeFormat::FAudio>();
			Audio.SampleRate = static_cast<uint32>(SampleRate);
			Audio.NumChannels = static_cast<uint32>(NumChannels);
			Audio.ChannelConfiguration = static_cast<uint32>(NumChannels);
			ElectraDecodersUtil::PrepareCodecTypeFormat(Format);

			TMap<FString, FVariant> Config;
			TMap<FString, FVariant> FormatInfo;
			TSharedPtr<IElectraCodecFactory, ESPMode::ThreadSafe> Factory = FactoryModule->GetBestDecoderFactoryForFormat(FormatInfo, Format, Config);
			if (!Factory.IsValid()) {
				LastError = TEXT("no AAC decoder available");
				return false;
			}
			Decoder = Factory->CreateDecoder(Format, Config);
			if (!Decoder.IsValid() || Decoder->GetError().IsSet()) {
				LastError = Decoder.IsValid() ? Decoder->GetError().GetMessage() : FString(TEXT("failed to create the AAC decoder"));
				Close();
				return false;
			}
			Bitstream = Decoder->CreateBitstreamProcessor();
			Name = FString::Printf(TEXT("Electra AAC (%s)"), *Factory->GetProviderInformation().GetName());
			if (!Pool.IsValid()) {
				Pool = MakeShareable(new FElectraAudioSamplePool);
			}
			return true;
		}

		virtual EIPDecodeResult Decode(const FIPAccessUnit& AccessUnit) override {
			if (!Decoder.IsValid()) {
				LastError = TEXT("decoder is not configured");
				return EIPDecodeResult::Error;
			}
			if (!Pending.bValid) {
				Pending.Copy = TArray<uint8>(AccessUnit.Data, AccessUnit.Size);
				Pending.DecAU = FElectraDecoderInputAccessUnit();
				Pending.DecAU.Data = Pending.Copy.GetData();
				Pending.DecAU.DataSize = Pending.Copy.Num();
				Pending.DecAU.DTS = AccessUnit.Dts;
				Pending.DecAU.PTS = AccessUnit.Pts;
				Pending.DecAU.Duration = AccessUnit.Duration;
				Pending.DecAU.UserValue = NextUserValue;
				Pending.DecAU.Flags = EElectraDecoderFlags::IsSyncSample;
				Pending.Options = Csd;
				Pending.Bsi.Reset();
				if (Bitstream.IsValid() &&
				    Bitstream->ProcessInputForDecoding(Pending.Bsi, Pending.DecAU, Pending.Options) == IElectraDecoderBitstreamProcessor::EProcessResult::Error) {
					LastError = Bitstream->GetLastError();
					return EIPDecodeResult::Error;
				}
				Pending.bValid = true;
			}
			const IElectraDecoder::EDecoderError Result = Decoder->DecodeAccessUnit(Pending.DecAU, Pending.Options);
			if (Result == IElectraDecoder::EDecoderError::NoBuffer) {
				return EIPDecodeResult::Busy;
			}
			Pending.bValid = false;
			if (Result != IElectraDecoder::EDecoderError::None) {
				LastError = Decoder->GetError().IsSet() ? Decoder->GetError().GetMessage() : FString(TEXT("AAC decode error"));
				return EIPDecodeResult::Error;
			}
			InFlight.Add(NextUserValue++, FInFlight{AccessUnit.Pts, AccessUnit.Duration});
			if (InFlight.Num() > 256) {
				InFlight.Empty();
			}
			return EIPDecodeResult::Ok;
		}

		virtual bool GetOutput(TSharedPtr<IMediaAudioSample, ESPMode::ThreadSafe>& OutSample) override {
			if (!Decoder.IsValid()) {
				return false;
			}
			for (;;) {
				if (Decoder->HaveOutput() != IElectraDecoder::EOutputStatus::Available) {
					return false;
				}
				TSharedPtr<IElectraDecoderOutput, ESPMode::ThreadSafe> Output = Decoder->GetOutput();
				if (!Output.IsValid() || Output->GetType() != IElectraDecoderOutput::EType::Audio) {
					continue;
				}
				FInFlight Input;
				if (!InFlight.RemoveAndCopyValue(Output->GetUserValue(), Input)) {
					Input.Pts = Output->GetPTS();
				}
				TSharedPtr<IElectraDecoderAudioOutput, ESPMode::ThreadSafe> AudioOutput = StaticCastSharedPtr<IElectraDecoderAudioOutput>(Output);
				if (!AudioOutput->IsInterleaved() || AudioOutput->GetSampleFormat() != IElectraDecoderAudioOutput::ESampleFormat::Float) {
					LastError = TEXT("unsupported audio output format");
					continue;
				}
				if (!ChannelMapper.IsInitialized() && !ChannelMapper.Initialize(AudioOutput)) {
					LastError = TEXT("could not initialize the audio channel mapper");
					continue;
				}
				const int32 NumFrames = AudioOutput->GetNumFrames();
				const int32 NumChannels = AudioOutput->GetNumChannels();
				const int32 SampleRate = AudioOutput->GetSampleRate();
				if (NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0) {
					continue;
				}
				FElectraAudioSampleRef Sample = Pool->AcquireShared();
				if (!Sample->AllocateFor(EMediaAudioSampleFormat::Float, static_cast<uint32>(NumChannels), static_cast<uint32>(NumFrames))) {
					continue;
				}
				const FTimespan Duration = FTimespan::FromSeconds(static_cast<double>(NumFrames) / SampleRate);
				Sample->SetParameters(static_cast<uint32>(SampleRate), FMediaTimeStamp(Input.Pts, 0, 0), Duration);
				ChannelMapper.MapChannels(Sample->GetWritableBuffer(), static_cast<int32>(Sample->GetAllocatedSize()),
					AudioOutput->GetData(0), NumFrames * AudioOutput->GetBytesPerFrame(), NumFrames);
				OutSample = Sample;
				return true;
			}
		}

		virtual void Close() override {
			if (Decoder.IsValid()) {
				Decoder->Close();
				Decoder.Reset();
			}
			Bitstream.Reset();
			Pending = FPendingAccessUnit();
			InFlight.Empty();
			ChannelMapper = Electra::FAudioChannelMapper();
		}

	private:
		TSharedPtr<IElectraDecoder, ESPMode::ThreadSafe> Decoder;
		TSharedPtr<IElectraDecoderBitstreamProcessor, ESPMode::ThreadSafe> Bitstream;
		TSharedPtr<FElectraAudioSamplePool, ESPMode::ThreadSafe> Pool;
		Electra::FAudioChannelMapper ChannelMapper;
		TMap<FString, FVariant> Csd;
		TArray<uint8> CurrentConfig;
		FPendingAccessUnit Pending;
		TMap<uint64, FInFlight> InFlight;
		uint64 NextUserValue = 1;
		FString Name = TEXT("Electra AAC");
		FString LastError;
	};

	// -----------------------------------------------------------------------------------------------------
	// Factory registry

	namespace {
		FCriticalSection FactoryLock;
		FIPVideoDecoderFactory& VideoFactory() {
			static FIPVideoDecoderFactory Factory;
			return Factory;
		}
		FIPAudioDecoderFactory& AudioFactory() {
			static FIPAudioDecoderFactory Factory;
			return Factory;
		}
	}
}

using namespace IPMedia;

void FIPMediaDecoders::SetVideoDecoderFactory(FIPVideoDecoderFactory Factory) {
	FScopeLock ScopeLock(&FactoryLock);
	VideoFactory() = MoveTemp(Factory);
}

void FIPMediaDecoders::SetAudioDecoderFactory(FIPAudioDecoderFactory Factory) {
	FScopeLock ScopeLock(&FactoryLock);
	AudioFactory() = MoveTemp(Factory);
}

TSharedPtr<IIPVideoDecoder, ESPMode::ThreadSafe> FIPMediaDecoders::CreateVideoDecoder() {
	FIPVideoDecoderFactory Factory;
	{
		FScopeLock ScopeLock(&FactoryLock);
		Factory = VideoFactory();
	}
	return Factory ? Factory() : TSharedPtr<IIPVideoDecoder, ESPMode::ThreadSafe>(MakeShared<FElectraVideoDecoder, ESPMode::ThreadSafe>());
}

TSharedPtr<IIPAudioDecoder, ESPMode::ThreadSafe> FIPMediaDecoders::CreateAudioDecoder() {
	FIPAudioDecoderFactory Factory;
	{
		FScopeLock ScopeLock(&FactoryLock);
		Factory = AudioFactory();
	}
	return Factory ? Factory() : TSharedPtr<IIPAudioDecoder, ESPMode::ThreadSafe>(MakeShared<FElectraAudioDecoder, ESPMode::ThreadSafe>());
}
