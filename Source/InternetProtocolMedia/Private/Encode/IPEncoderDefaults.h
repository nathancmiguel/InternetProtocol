/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "IPMediaEncoders.h"

namespace IPMedia {
	/** AVCodecs hardware H.264 (NVENC / AMF). */
	TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> CreateDefaultVideoEncoder();
	/** Media Foundation AAC-LC. */
	TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> CreateDefaultAudioEncoder();
}
