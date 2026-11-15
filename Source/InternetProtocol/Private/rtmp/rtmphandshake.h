/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include <vector>

/*
 * RTMP "simple" handshake (RTMP spec section 5.2): C0/C1/C2 <-> S0/S1/S2, 1 + 1536 + 1536 bytes.
 * The Flash "complex" (digest) handshake is not required by OBS, FFmpeg, nginx-rtmp, MediaMTX, Twitch or
 * YouTube, so it is not implemented. Bytes that arrive after the handshake belong to the chunk stream and
 * are returned to the caller.
 */
namespace IPNet::Rtmp {

	constexpr size_t HandshakeSize = 1536;

	class FRtmpHandshake {
	public:
		enum class ERole : uint8 { Client, Server };
		enum class EResult : uint8 { NeedMore, Done, Error };

		explicit FRtmpHandshake(ERole InRole);

		/** Client: C0+C1 to send immediately. Server: empty. */
		std::vector<uint8> Start();

		/**
		 * Feeds received bytes. OutToSend receives the bytes to write back (may be empty).
		 * When Done, OutLeftover holds any bytes received after the handshake.
		 */
		EResult Feed(const uint8* Data, size_t Size, std::vector<uint8>& OutToSend, std::vector<uint8>& OutLeftover);

		bool IsDone() const { return State == EState::Done; }

	private:
		enum class EState : uint8 { Initial, WaitPeerHello, WaitPeerAck, Done, Failed };

		std::vector<uint8> MakeHello() const;

		ERole Role;
		EState State = EState::Initial;
		std::vector<uint8> Buffer;
	};
}
