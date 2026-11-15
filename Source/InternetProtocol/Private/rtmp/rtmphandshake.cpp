/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmphandshake.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"

namespace IPNet::Rtmp {
	namespace {
		constexpr uint8 RtmpVersion = 3;
	}

	FRtmpHandshake::FRtmpHandshake(ERole InRole) : Role(InRole) {}

	std::vector<uint8> FRtmpHandshake::MakeHello() const {
		// time(4) + zero(4) + 1528 random bytes. Randomness is not security relevant here.
		std::vector<uint8> Hello(HandshakeSize, 0);
		const uint32 Time = static_cast<uint32>(FPlatformTime::Cycles64() / 1000);
		Hello[0] = static_cast<uint8>(Time >> 24);
		Hello[1] = static_cast<uint8>(Time >> 16);
		Hello[2] = static_cast<uint8>(Time >> 8);
		Hello[3] = static_cast<uint8>(Time);
		FRandomStream Random(static_cast<int32>(FPlatformTime::Cycles()));
		for (size_t Index = 8; Index < HandshakeSize; ++Index) {
			Hello[Index] = static_cast<uint8>(Random.RandRange(0, 255));
		}
		return Hello;
	}

	std::vector<uint8> FRtmpHandshake::Start() {
		if (Role != ERole::Client || State != EState::Initial) {
			return {};
		}
		std::vector<uint8> Out;
		Out.reserve(1 + HandshakeSize);
		Out.push_back(RtmpVersion);
		const std::vector<uint8> C1 = MakeHello();
		Out.insert(Out.end(), C1.begin(), C1.end());
		State = EState::WaitPeerHello;
		return Out;
	}

	FRtmpHandshake::EResult FRtmpHandshake::Feed(const uint8* Data, size_t Size, std::vector<uint8>& OutToSend, std::vector<uint8>& OutLeftover) {
		OutToSend.clear();
		OutLeftover.clear();
		if (State == EState::Failed) {
			return EResult::Error;
		}
		if (State == EState::Done) {
			OutLeftover.assign(Data, Data + Size);
			return EResult::Done;
		}
		if (Role == ERole::Server && State == EState::Initial) {
			State = EState::WaitPeerHello;
		}
		Buffer.insert(Buffer.end(), Data, Data + Size);

		if (State == EState::WaitPeerHello) {
			// Client waits for S0+S1+S2 (we answer with C2); server waits for C0+C1 (we answer S0+S1+S2).
			const size_t Needed = Role == ERole::Client ? 1 + 2 * HandshakeSize : 1 + HandshakeSize;
			if (Buffer.size() < Needed) {
				return EResult::NeedMore;
			}
			if (Buffer[0] != RtmpVersion) {
				State = EState::Failed;
				return EResult::Error;
			}
			const uint8* PeerHello = Buffer.data() + 1;
			if (Role == ERole::Client) {
				// C2 echoes S1.
				OutToSend.assign(PeerHello, PeerHello + HandshakeSize);
				OutLeftover.assign(Buffer.begin() + static_cast<std::ptrdiff_t>(Needed), Buffer.end());
				Buffer = std::vector<uint8>();
				State = EState::Done;
				return EResult::Done;
			}
			OutToSend.reserve(1 + 2 * HandshakeSize);
			OutToSend.push_back(RtmpVersion);
			const std::vector<uint8> S1 = MakeHello();
			OutToSend.insert(OutToSend.end(), S1.begin(), S1.end());
			OutToSend.insert(OutToSend.end(), PeerHello, PeerHello + HandshakeSize); // S2 echoes C1
			Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Needed));
			State = EState::WaitPeerAck;
		}

		if (State == EState::WaitPeerAck) {
			if (Buffer.size() < HandshakeSize) {
				return EResult::NeedMore;
			}
			// C2 content is not validated: several encoders do not echo S1 byte-exact.
			OutLeftover.assign(Buffer.begin() + static_cast<std::ptrdiff_t>(HandshakeSize), Buffer.end());
			Buffer = std::vector<uint8>();
			State = EState::Done;
			return EResult::Done;
		}
		return EResult::NeedMore;
	}
}
