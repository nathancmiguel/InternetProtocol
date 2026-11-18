/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "IPMediaEncoders.h"

namespace IPMedia {
	/** AVCodecs hardware H.264 (NVENC / AMF). */
	TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> CreateDefaultVideoEncoder();
	/**
	 * Whether the default video encoder can run with the current RHI. AVCodecs' D3D11 path removes the D3D11
	 * device when it fails to open (fatal GPU crash), so only D3D12 is allowed. OutReason explains a refusal.
	 */
	bool CanUseDefaultVideoEncoder(FString& OutReason);
	/** Media Foundation AAC-LC. */
	TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> CreateDefaultAudioEncoder();
}
