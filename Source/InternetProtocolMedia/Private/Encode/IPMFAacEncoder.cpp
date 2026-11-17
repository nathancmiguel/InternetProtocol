/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Encode/IPEncoderDefaults.h"

#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#include "Microsoft/COMPointer.h"

/*
 * AAC-LC through the Microsoft Media Foundation AAC encoder MFT (the codec behind AVCodecs' FAudioEncoderWMF).
 * It is driven directly because FAudioEncoderWMF reads at most one output per input and reports the normal
 * "needs more input" state as an error, which loses or misreports frames.
 *
 * Constraints of the Microsoft encoder: 16-bit PCM, 44.1/48 kHz, 1 or 2 channels, 96/128/160/192 kbps.
 * Output is raw AAC (no ADTS), one access unit = 1024 samples per channel.
 */
namespace IPMedia {
	namespace {
		int32 SampleRateIndex(int32 Rate) {
			static const int32 Rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Rates); ++Index) {
				if (Rates[Index] == Rate) {
					return Index;
				}
			}
			return -1;
		}

		class FMFAacEncoder final : public IIPAudioEncoder {
		public:
			virtual ~FMFAacEncoder() override { Close(); }

			virtual FString GetName() const override { return TEXT("Media Foundation AAC-LC"); }
			virtual FString GetLastError() const override { return LastError; }

			virtual bool Open(const FIPAudioEncoderSettings& InSettings) override {
				Close();
				Settings = InSettings;
				if (Settings.SampleRate != 44100 && Settings.SampleRate != 48000) {
					LastError = FString::Printf(TEXT("unsupported AAC sample rate %d (44100 or 48000)"), Settings.SampleRate);
					return false;
				}
				if (Settings.NumChannels != 1 && Settings.NumChannels != 2) {
					LastError = FString::Printf(TEXT("unsupported AAC channel count %d (1 or 2)"), Settings.NumChannels);
					return false;
				}
				static const int32 Bitrates[] = {96, 128, 160, 192};
				int32 Best = Bitrates[0];
				for (int32 Candidate : Bitrates) {
					if (FMath::Abs(Candidate - Settings.BitrateKbps) < FMath::Abs(Best - Settings.BitrateKbps)) {
						Best = Candidate;
					}
				}
				Settings.BitrateKbps = Best;

				if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
					LastError = TEXT("Media Foundation is not available");
					return false;
				}
				bMFStarted = true;

				HRESULT Result = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Encoder));
				if (FAILED(Result)) {
					return Fail(TEXT("the Media Foundation AAC encoder is not installed"), Result);
				}
				TComPtr<IMFMediaType> InputType;
				if (FAILED(Result = MFCreateMediaType(&InputType)) ||
				    FAILED(Result = InputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
				    FAILED(Result = InputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
				    FAILED(Result = InputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
				    FAILED(Result = InputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(Settings.SampleRate))) ||
				    FAILED(Result = InputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(Settings.NumChannels))) ||
				    FAILED(Result = Encoder->SetInputType(0, InputType, 0))) {
					return Fail(TEXT("AAC encoder rejected the PCM input format"), Result);
				}
				TComPtr<IMFMediaType> OutputType;
				if (FAILED(Result = MFCreateMediaType(&OutputType)) ||
				    FAILED(Result = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
				    FAILED(Result = OutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC)) ||
				    FAILED(Result = OutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
				    FAILED(Result = OutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(Settings.SampleRate))) ||
				    FAILED(Result = OutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(Settings.NumChannels))) ||
				    FAILED(Result = OutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(Settings.BitrateKbps * 1000 / 8))) ||
				    FAILED(Result = OutputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)) ||
				    FAILED(Result = Encoder->SetOutputType(0, OutputType, 0))) {
					return Fail(TEXT("AAC encoder rejected the output format"), Result);
				}
				if (FAILED(Result = Encoder->GetOutputStreamInfo(0, &OutputInfo)) ||
				    FAILED(Result = Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
				    FAILED(Result = Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
					return Fail(TEXT("AAC encoder failed to start"), Result);
				}
				return true;
			}

			virtual TArray<uint8> GetAudioSpecificConfig() const override {
				// AAC-LC (object type 2), sampling frequency index, channel configuration.
				const int32 FreqIndex = SampleRateIndex(Settings.SampleRate);
				TArray<uint8> Asc;
				Asc.Add(static_cast<uint8>((2 << 3) | ((FreqIndex >> 1) & 0x07)));
				Asc.Add(static_cast<uint8>(((FreqIndex & 0x01) << 7) | ((Settings.NumChannels & 0x0F) << 3)));
				return Asc;
			}

			virtual bool Encode(const int16* Interleaved, int32 NumFrames, int64 TimestampMs) override {
				if (!Encoder.IsValid()) {
					LastError = TEXT("encoder is not open");
					return false;
				}
				if (NumFrames <= 0) {
					return true;
				}
				const DWORD Bytes = static_cast<DWORD>(NumFrames * Settings.NumChannels * sizeof(int16));
				TComPtr<IMFSample> Sample;
				TComPtr<IMFMediaBuffer> Buffer;
				HRESULT Result;
				if (FAILED(Result = MFCreateSample(&Sample)) || FAILED(Result = MFCreateMemoryBuffer(Bytes, &Buffer))) {
					return Fail(TEXT("could not allocate an AAC input buffer"), Result, false);
				}
				BYTE* Data = nullptr;
				if (FAILED(Result = Buffer->Lock(&Data, nullptr, nullptr))) {
					return Fail(TEXT("could not lock an AAC input buffer"), Result, false);
				}
				FMemory::Memcpy(Data, Interleaved, Bytes);
				Buffer->Unlock();
				Buffer->SetCurrentLength(Bytes);
				Sample->AddBuffer(Buffer);
				Sample->SetSampleTime(TimestampMs * 10000);
				Sample->SetSampleDuration(static_cast<LONGLONG>(NumFrames) * 10000000 / Settings.SampleRate);

				Result = Encoder->ProcessInput(0, Sample, 0);
				if (Result == MF_E_NOTACCEPTING) {
					// Output is pending: drain it, then the input is accepted.
					if (!DrainOutputs()) {
						return false;
					}
					Result = Encoder->ProcessInput(0, Sample, 0);
				}
				if (FAILED(Result)) {
					return Fail(TEXT("AAC encode failed"), Result, false);
				}
				return DrainOutputs();
			}

			virtual bool Receive(FIPEncodedAudioFrame& Out) override {
				if (Pending.Num() == 0) {
					return false;
				}
				Out = MoveTemp(Pending[0]);
				Pending.RemoveAt(0, EAllowShrinking::No);
				return true;
			}

			virtual void Close() override {
				if (Encoder.IsValid()) {
					Encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
					Encoder.Reset();
				}
				Pending.Reset();
				if (bMFStarted) {
					MFShutdown();
					bMFStarted = false;
				}
			}

		private:
			bool DrainOutputs() {
				for (;;) {
					TComPtr<IMFSample> Sample;
					TComPtr<IMFMediaBuffer> Buffer;
					HRESULT Result;
					const DWORD Size = FMath::Max<DWORD>(OutputInfo.cbSize, 8192);
					if (FAILED(Result = MFCreateSample(&Sample)) || FAILED(Result = MFCreateMemoryBuffer(Size, &Buffer))) {
						return Fail(TEXT("could not allocate an AAC output buffer"), Result, false);
					}
					Sample->AddBuffer(Buffer);
					MFT_OUTPUT_DATA_BUFFER Output = {};
					Output.pSample = Sample;
					DWORD Status = 0;
					Result = Encoder->ProcessOutput(0, 1, &Output, &Status);
					if (Output.pEvents) {
						Output.pEvents->Release();
					}
					if (Result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
						return true;
					}
					if (FAILED(Result)) {
						return Fail(TEXT("AAC encoder output failed"), Result, false);
					}
					BYTE* Data = nullptr;
					DWORD Length = 0;
					if (SUCCEEDED(Buffer->Lock(&Data, nullptr, &Length))) {
						if (Length > 0) {
							FIPEncodedAudioFrame& Frame = Pending.AddDefaulted_GetRef();
							Frame.Data = TArray<uint8>(Data, static_cast<int32>(Length));
							LONGLONG Time = 0;
							Frame.TimestampMs = SUCCEEDED(Sample->GetSampleTime(&Time)) ? Time / 10000 : 0;
						}
						Buffer->Unlock();
					}
					// Bounded: the publisher takes outputs after every Encode().
					if (Pending.Num() > 256) {
						Pending.RemoveAt(0);
					}
				}
			}

			bool Fail(const TCHAR* What, HRESULT Result, bool bClose = true) {
				LastError = FString::Printf(TEXT("%s (0x%08x)"), What, static_cast<uint32>(Result));
				if (bClose) {
					Close();
				}
				return false;
			}

			FIPAudioEncoderSettings Settings;
			TComPtr<IMFTransform> Encoder;
			MFT_OUTPUT_STREAM_INFO OutputInfo = {};
			TArray<FIPEncodedAudioFrame> Pending;
			bool bMFStarted = false;
			FString LastError;
		};
	}

	TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> CreateDefaultAudioEncoder() {
		return MakeShared<FMFAacEncoder, ESPMode::ThreadSafe>();
	}
}
