/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Encode/IPEncoderDefaults.h"
#include "AVDevice.h"
#include "AVResult.h"
#include "RHI.h"
#include "Video/VideoEncoder.h"
#include "Video/Encoders/Configs/VideoEncoderConfigH264.h"
#include "Video/Resources/VideoResourceRHI.h"

/*
 * H.264 through the engine's AVCodecs (Experimental): the hardware encoders registered by the NVCodecs (NVENC)
 * and AMFCodecs plugins, fed with RHI textures (no CPU readback). Low-latency mode: no B-frames, so decode
 * order is presentation order and the FLV composition time is always 0. There is no software fallback in the
 * engine: without a supported NVIDIA/AMD GPU, Open() fails with a clear error.
 */
namespace IPMedia {
	namespace {
		class FAVCodecsVideoEncoder final : public IIPVideoEncoder {
		public:
			virtual ~FAVCodecsVideoEncoder() override { Close(); }

			virtual FString GetName() const override { return Name; }
			virtual FString GetLastError() const override { return LastError; }

			virtual bool Open(const FIPVideoEncoderSettings& InSettings) override {
				Close();
				Settings = InSettings;
				if (Settings.Width < 16 || Settings.Height < 16 || (Settings.Width % 2) != 0 || (Settings.Height % 2) != 0) {
					LastError = FString::Printf(TEXT("invalid video size %dx%d (even values, at least 16)"), Settings.Width, Settings.Height);
					return false;
				}
				// Never touch AVCodecs outside D3D12: on D3D11 a failed encoder creation removes the device.
				if (!CanUseDefaultVideoEncoder(LastError)) {
					return false;
				}
				FVideoEncoderConfigH264 Config(EAVPreset::Default);
				Config.LatencyMode = EAVLatencyMode::LowLatency;
				Config.Width = static_cast<uint32>(Settings.Width);
				Config.Height = static_cast<uint32>(Settings.Height);
				Config.TargetFramerate = static_cast<uint32>(FMath::Clamp(Settings.FrameRate, 1, 240));
				Config.TargetBitrate = FMath::Clamp(Settings.BitrateKbps, 100, 200000) * 1000;
				Config.MaxBitrate = Config.TargetBitrate;
				Config.MinBitrate = Config.TargetBitrate / 2;
				Config.RateControlMode = ERateControlMode::CBR;
				Config.bFillData = false;
				Config.KeyframeInterval = Config.TargetFramerate * static_cast<uint32>(FMath::Clamp(Settings.KeyframeIntervalSeconds, 1, 10));
				Config.MultipassMode = EMultipassMode::Quarter;
				Config.Profile = EH264Profile::High;
				// SPS/PPS before every IDR: the publisher builds avcC from the first one and late joiners need them.
				Config.RepeatSPSPPS = true;

				Encoder = FVideoEncoder::Create<FVideoResourceRHI>(FAVDevice::GetHardwareDevice(), Config);
				if (!Encoder.IsValid() || !Encoder->IsOpen()) {
					Encoder.Reset();
					LastError = TEXT("no hardware H.264 encoder (NVENC/AMF) is available on this GPU/driver");
					return false;
				}
				Name = FString::Printf(TEXT("AVCodecs H.264 (%s)"), IsRHIDeviceNVIDIA() ? TEXT("NVENC") : IsRHIDeviceAMD() ? TEXT("AMF") : TEXT("hardware"));
				return true;
			}

			virtual bool Encode(const FTextureRHIRef& Texture, int64 TimestampMs, bool bForceKeyframe) override {
				if (!Encoder.IsValid()) {
					LastError = TEXT("encoder is not open");
					return false;
				}
				if (!Texture.IsValid()) {
					LastError = TEXT("no input texture");
					return false;
				}
				TSharedPtr<FVideoResourceRHI> Resource = MakeShared<FVideoResourceRHI>(
					Encoder->GetDevice().ToSharedRef(), FVideoResourceRHI::FRawData{Texture, nullptr, 0});
				// AVCodecs timestamps are 32-bit milliseconds: wraps after ~49 days, irrelevant for a session.
				const FAVResult Result = Encoder->SendFrame(Resource, static_cast<uint32>(TimestampMs), bForceKeyframe);
				if (Result.IsNotSuccess()) {
					LastError = Result.Message.IsEmpty() ? FString(TEXT("H.264 encode failed")) : Result.Message;
					return false;
				}
				return true;
			}

			virtual bool Receive(FIPEncodedVideoFrame& Out) override {
				if (!Encoder.IsValid()) {
					return false;
				}
				FVideoPacket Packet;
				if (!Encoder->ReceivePacket(Packet) || Packet.IsEmpty()) {
					return false;
				}
				Out.AnnexB = TArray<uint8>(Packet.DataPtr.Get(), static_cast<int32>(Packet.DataSize));
				Out.TimestampMs = static_cast<int64>(Packet.Timestamp);
				Out.bKeyframe = Packet.bIsKeyframe != 0;
				return true;
			}

			virtual void Close() override {
				Encoder.Reset();
			}

		private:
			FIPVideoEncoderSettings Settings;
			TSharedPtr<TVideoEncoder<FVideoResourceRHI>> Encoder;
			FString Name = TEXT("AVCodecs H.264");
			FString LastError;
		};
	}

	bool CanUseDefaultVideoEncoder(FString& OutReason) {
		if (!GDynamicRHI) {
			OutReason = TEXT("hardware H.264 publishing needs a GPU (no RHI in this process)");
			return false;
		}
		if (RHIGetInterfaceType() != ERHIInterfaceType::D3D12) {
			OutReason = FString::Printf(TEXT("hardware H.264 publishing needs the D3D12 RHI (current: %s); use DirectX 12 with SM5 or SM6 shader formats"),
				GDynamicRHI->GetName());
			return false;
		}
		return true;
	}

	TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> CreateDefaultVideoEncoder() {
		return MakeShared<FAVCodecsVideoEncoder, ESPMode::ThreadSafe>();
	}
}
