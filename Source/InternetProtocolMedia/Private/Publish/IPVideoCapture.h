/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "RHIResources.h"
#include <atomic>

class FRHICommandListImmediate;

/*
 * GPU side of video capture for publishing. Each captured frame is drawn (scaled, converted to 8-bit BGRA and
 * gamma-encoded when the source is linear) into one of a few shared textures the hardware encoder reads
 * directly: no GPU->CPU readback. A GPU fence written after the draw tells the encode thread when the picture is
 * complete. If every slot is still waiting for the encoder, the frame is dropped (counted), never queued.
 *
 * Threading: Capture() runs on the render thread; TryTake()/Release() on the encode thread.
 */
namespace IPMedia {

	struct FCapturedFrame {
		int32 Slot = INDEX_NONE;
		FTextureRHIRef Texture;
		FGPUFenceRHIRef Fence;
		int64 TimestampMs = 0;
		double CapturedAt = 0.0;
	};

	class FVideoCapturePool : public TSharedFromThis<FVideoCapturePool, ESPMode::ThreadSafe> {
	public:
		static constexpr int32 NumSlots = 3;

		FVideoCapturePool(int32 InWidth, int32 InHeight);
		~FVideoCapturePool();

		/**
		 * Render thread. Draws Source into a free slot and queues it for the encoder. SourceEndState is the state the
		 * source is left in (ERHIAccess::Present for a back buffer, SRVMask for render targets).
		 */
		bool Capture(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, int64 TimestampMs, ERHIAccess SourceEndState);

		/** Encode thread. Oldest captured frame whose GPU work is complete (frames stuck > 1 s are dropped). */
		bool TryTake(FCapturedFrame& Out);
		/** Encode thread. Makes the slot available again once the encoder is done with it. */
		void Release(int32 Slot);

		int32 GetWidth() const { return Width; }
		int32 GetHeight() const { return Height; }
		int64 GetDroppedFrames() const { return DroppedFrames.load(); }

	private:
		const int32 Width;
		const int32 Height;
		FTextureRHIRef Textures[NumSlots];
		std::atomic<bool> InUse[NumSlots];
		std::atomic<int64> DroppedFrames{0};
		FCriticalSection QueueLock;
		TArray<FCapturedFrame> Queue;
	};
}
