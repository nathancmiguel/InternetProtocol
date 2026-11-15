/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "rtmp/rtmpmedia.h"
#include "rtmp/rtmppublish.h"
#include "rtmp/rtmpserver.h"
#include "rtmp/flv.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
THIRD_PARTY_INCLUDES_END
#undef UI

using namespace IPNet::Rtmp;

namespace {
	constexpr EAutomationTestFlags RtmpTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	class FTestSink final : public IRtmpMediaSink {
	public:
		virtual void OnStreamStarted(const FRtmpStreamInfo& Info) override {
			FScopeLock ScopeLock(&Lock);
			++Started;
			StartedInfo = Info;
		}
		virtual void OnMediaPacket(const FRtmpMediaPacket& Packet) override {
			FScopeLock ScopeLock(&Lock);
			Packets.Add(Packet);
		}
		virtual void OnStreamEnded() override {
			FScopeLock ScopeLock(&Lock);
			++Ended;
		}

		int32 Count(ERtmpPacketType Type, bool bHeader) {
			FScopeLock ScopeLock(&Lock);
			int32 Result = 0;
			for (const FRtmpMediaPacket& Packet : Packets) {
				Result += Packet.Type == Type && Packet.bSequenceHeader == bHeader ? 1 : 0;
			}
			return Result;
		}

		FCriticalSection Lock;
		int32 Started = 0;
		int32 Ended = 0;
		FRtmpStreamInfo StartedInfo;
		TArray<FRtmpMediaPacket> Packets;
	};

	void Wait(FAutomationTestBase* Test, const FString& What, TFunction<bool()> Predicate, double Timeout = 10.0) {
		TSharedRef<double> Start = MakeShared<double>(-1.0);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, What, Predicate, Timeout, Start]() {
			if (*Start < 0.0) {
				*Start = FPlatformTime::Seconds();
			}
			if (Predicate()) {
				return true;
			}
			if (FPlatformTime::Seconds() - *Start > Timeout) {
				Test->AddError(FString::Printf(TEXT("Timed out waiting for: %s"), *What));
				return true;
			}
			return false;
		}));
	}

	void Then(TFunction<void()> Step) {
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() {
			Step();
			return true;
		}));
	}

	bool MakeCertificate(FString& OutCert, FString& OutKey) {
		EVP_PKEY_CTX* KeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		EVP_PKEY* Key = nullptr;
		bool bOk = KeyCtx && EVP_PKEY_keygen_init(KeyCtx) == 1 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(KeyCtx, NID_X9_62_prime256v1) == 1 &&
		           EVP_PKEY_keygen(KeyCtx, &Key) == 1;
		EVP_PKEY_CTX_free(KeyCtx);
		X509* Cert = bOk ? X509_new() : nullptr;
		if (Cert) {
			X509_set_version(Cert, 2);
			ASN1_INTEGER_set(X509_get_serialNumber(Cert), 1);
			X509_gmtime_adj(X509_getm_notBefore(Cert), -3600);
			X509_gmtime_adj(X509_getm_notAfter(Cert), 86400);
			X509_set_pubkey(Cert, Key);
			X509_NAME* Name = X509_get_subject_name(Cert);
			X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
			X509_set_issuer_name(Cert, Name);
			X509V3_CTX V3;
			X509V3_set_ctx_nodb(&V3);
			X509V3_set_ctx(&V3, Cert, Cert, nullptr, nullptr, 0);
			if (X509_EXTENSION* San = X509V3_EXT_conf_nid(nullptr, &V3, NID_subject_alt_name, const_cast<char*>("DNS:localhost,IP:127.0.0.1"))) {
				X509_add_ext(Cert, San, -1);
				X509_EXTENSION_free(San);
			}
			bOk = X509_sign(Cert, Key, EVP_sha256()) > 0;
		}
		if (bOk) {
			BIO* CertBio = BIO_new(BIO_s_mem());
			BIO* KeyBio = BIO_new(BIO_s_mem());
			PEM_write_bio_X509(CertBio, Cert);
			PEM_write_bio_PrivateKey(KeyBio, Key, nullptr, nullptr, 0, nullptr, nullptr);
			char* Data = nullptr;
			long Len = BIO_get_mem_data(CertBio, &Data);
			OutCert = FString(static_cast<int32>(Len), ANSI_TO_TCHAR(Data));
			Len = BIO_get_mem_data(KeyBio, &Data);
			OutKey = FString(static_cast<int32>(Len), ANSI_TO_TCHAR(Data));
			BIO_free(CertBio);
			BIO_free(KeyBio);
		}
		X509_free(Cert);
		EVP_PKEY_free(Key);
		return bOk;
	}

	TArray<uint8> FakeAvcC() {
		const uint8 Sps[] = {0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9, 0x40, 0x50, 0x05, 0xBB, 0x01, 0x10};
		const uint8 Pps[] = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};
		FAvcConfig Config;
		Config.Sps.emplace_back(Sps, Sps + sizeof(Sps));
		Config.Pps.emplace_back(Pps, Pps + sizeof(Pps));
		const std::vector<uint8> Record = BuildAvcDecoderConfig(Config);
		return TArray<uint8>(Record.data(), static_cast<int32>(Record.size()));
	}

	TArray<uint8> FakeFrame(int32 Size, uint8 NalType) {
		TArray<uint8> Frame;
		Frame.SetNumZeroed(Size + 4);
		Frame[0] = static_cast<uint8>(Size >> 24);
		Frame[1] = static_cast<uint8>(Size >> 16);
		Frame[2] = static_cast<uint8>(Size >> 8);
		Frame[3] = static_cast<uint8>(Size);
		Frame[4] = NalType;
		for (int32 Index = 5; Index < Frame.Num(); ++Index) {
			Frame[Index] = static_cast<uint8>(Index * 13);
		}
		return Frame;
	}

	/** Shared body of the RTMP and RTMPS loopback tests. */
	void RunLoopback(FAutomationTestBase* Test, bool bTls) {
		FString Cert, Key;
		if (bTls && !MakeCertificate(Cert, Key)) {
			Test->AddError(TEXT("Could not create a test certificate"));
			return;
		}

		URTMPServer* Server = NewObject<URTMPServer>();
		Server->AddToRoot();
		Server->SetAllowedStreamKeys({TEXT("secret-key")});
		if (bTls) {
			FSecurityContextOpts ServerTls;
			ServerTls.Cert = Cert;
			ServerTls.Private_Key = Key;
			ServerTls.Verify_Mode = EVerifyMode::None;
			Server->SetTlsOptions(ServerTls);
		}
		FServerBindOptions Bind;
		Bind.Address = TEXT("127.0.0.1");
		Bind.Port = 0;
		if (!Test->TestTrue(TEXT("Server opens"), Server->Open(Bind))) {
			Server->RemoveFromRoot();
			return;
		}
		const uint16 Port = static_cast<uint16>(Server->LocalEndpoint().Port);

		TSharedRef<FTestSink, ESPMode::ThreadSafe> Sink = MakeShared<FTestSink, ESPMode::ThreadSafe>();
		const int64 SinkHandle = FRtmpIngestHub::RegisterSink(Port, TEXT("live"), TEXT("secret-key"), Sink);

		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Errors = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
		FRtmpPublishOptions Options;
		Options.Width = 1280;
		Options.Height = 720;
		Options.FrameRate = 30;
		Options.Tls.CA_Cert = Cert;
		Options.Tls.bUseSystemRootCertificates = false;
		FRtmpPublishCallbacks Callbacks;
		Callbacks.OnError = [Errors](const FString&) { Errors->fetch_add(1); };
		FString Error;
		const FString Url = FString::Printf(TEXT("%s://%s:%u/live"), bTls ? TEXT("rtmps") : TEXT("rtmp"), bTls ? TEXT("localhost") : TEXT("127.0.0.1"), Port);
		TSharedPtr<IRtmpPublishSession, ESPMode::ThreadSafe> Publisher = IRtmpPublishSession::Create(Url, TEXT("secret-key"), Options, Callbacks, Error);
		if (!Test->TestTrue(TEXT("Publisher created"), Publisher.IsValid())) {
			Test->AddError(Error);
			return;
		}
		Test->TestEqual(TEXT("Masked URL hides the key"), IRtmpPublishSession::MaskUrl(Url + TEXT("/secret-key")).Contains(TEXT("secret-key")), false);
		Publisher->Start();

		Wait(Test, TEXT("publishing"), [Publisher]() { return Publisher->GetState() == ERtmpPublishState::Publishing; });
		Then([Publisher]() {
			Publisher->SetVideoConfig(FakeAvcC());
			Publisher->SetAudioConfig(TArray<uint8>({0x11, 0x90}));
			// GOP: IDR + 5 P-frames with B-frame style composition offsets; 10 AAC frames.
			for (int32 Frame = 0; Frame < 6; ++Frame) {
				Publisher->SendVideo(Frame * 33, Frame == 0 ? 0 : 66, Frame == 0, FakeFrame(Frame == 0 ? 50000 : 3000, Frame == 0 ? 0x65 : 0x41));
			}
			for (int32 Frame = 0; Frame < 10; ++Frame) {
				Publisher->SendAudio(Frame * 21, TArray<uint8>({0x21, 0x10, static_cast<uint8>(Frame)}));
			}
		});
		Wait(Test, TEXT("all media received"), [Sink]() {
			return Sink->Count(ERtmpPacketType::Video, false) >= 6 && Sink->Count(ERtmpPacketType::Audio, false) >= 10;
		});
		Then([Test, Sink, Publisher, Errors]() {
			FScopeLock ScopeLock(&Sink->Lock);
			Test->TestEqual(TEXT("Stream started once"), Sink->Started, 1);
			Test->TestTrue(TEXT("Metadata width"), Sink->StartedInfo.Width == 1280.0 || Sink->StartedInfo.Width == 0.0);
			Test->TestEqual(TEXT("App"), Sink->StartedInfo.App, FString(TEXT("live")));
			int32 VideoIndex = 0;
			int64 LastDts = -1;
			bool bSawVideoHeader = false, bSawAudioHeader = false;
			for (const FRtmpMediaPacket& Packet : Sink->Packets) {
				if (Packet.bSequenceHeader) {
					(Packet.Type == ERtmpPacketType::Video ? bSawVideoHeader : bSawAudioHeader) = true;
					continue;
				}
				if (Packet.Type != ERtmpPacketType::Video) {
					continue;
				}
				Test->TestEqual(TEXT("Video codec"), static_cast<int32>(Packet.Codec), 7);
				Test->TestEqual(TEXT("Keyframe flag"), Packet.bKeyframe, VideoIndex == 0);
				Test->TestEqual(TEXT("Composition time"), Packet.CtsMs, VideoIndex == 0 ? 0 : 66);
				Test->TestEqual(TEXT("Payload size"), Packet.Payload->Num(), VideoIndex == 0 ? 50004 : 3004);
				Test->TestTrue(TEXT("DTS monotonic"), Packet.DtsMs > LastDts);
				LastDts = Packet.DtsMs;
				++VideoIndex;
			}
			Test->TestTrue(TEXT("avcC delivered"), bSawVideoHeader);
			Test->TestTrue(TEXT("AudioSpecificConfig delivered"), bSawAudioHeader);
			Test->TestEqual(TEXT("No publisher errors"), Errors->load(), 0);
			const FRtmpPublishStats Stats = Publisher->GetStats();
			Test->TestEqual(TEXT("Nothing dropped"), Stats.DroppedVideoFrames + Stats.DroppedAudioFrames, 0);
			Publisher->Stop();
		});
		Wait(Test, TEXT("stream ended"), [Sink]() {
			FScopeLock ScopeLock(&Sink->Lock);
			return Sink->Ended == 1;
		});
		Wait(Test, TEXT("publisher stopped"), [Publisher]() { return Publisher->GetState() == ERtmpPublishState::Stopped; });
		Then([Server, SinkHandle]() {
			FRtmpIngestHub::UnregisterSink(SinkHandle);
			Server->Close();
			Server->RemoveFromRoot();
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpLoopbackTest, "InternetProtocol.RTMP.Loopback.Plain", RtmpTestFlags)
bool FIPRtmpLoopbackTest::RunTest(const FString& Parameters) {
	RunLoopback(this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpsLoopbackTest, "InternetProtocol.RTMP.Loopback.Tls", RtmpTestFlags)
bool FIPRtmpsLoopbackTest::RunTest(const FString& Parameters) {
	RunLoopback(this, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIPRtmpRejectTest, "InternetProtocol.RTMP.Loopback.WrongKeyRejected", RtmpTestFlags)
bool FIPRtmpRejectTest::RunTest(const FString& Parameters) {
	URTMPServer* Server = NewObject<URTMPServer>();
	Server->AddToRoot();
	Server->SetAllowedStreamKeys({TEXT("right")});
	FServerBindOptions Bind;
	Bind.Address = TEXT("127.0.0.1");
	Bind.Port = 0;
	if (!TestTrue(TEXT("Server opens"), Server->Open(Bind))) {
		Server->RemoveFromRoot();
		return false;
	}
	FString Error;
	TSharedPtr<IRtmpPublishSession, ESPMode::ThreadSafe> Publisher = IRtmpPublishSession::Create(
		FString::Printf(TEXT("rtmp://127.0.0.1:%d/live/wrong"), Server->LocalEndpoint().Port), FString(), FRtmpPublishOptions(), FRtmpPublishCallbacks(), Error);
	if (!TestTrue(TEXT("Publisher created"), Publisher.IsValid())) {
		return false;
	}
	Publisher->Start();
	Wait(this, TEXT("publish refused without reconnecting"), [Publisher]() { return Publisher->GetState() == ERtmpPublishState::Failed; });
	Then([this, Publisher, Server]() {
		TestEqual(TEXT("No reconnection for a refused key"), Publisher->GetStats().ReconnectAttempts, 0);
		Server->Close();
		Server->RemoveFromRoot();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
