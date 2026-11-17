/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Publish/IPVideoCapture.h"
#include "CommonRenderResources.h"
#include "GlobalShader.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PipelineStateCache.h"
#include "RenderUtils.h"
#include "RendererInterface.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "ScreenRendering.h"

namespace IPMedia {
	namespace {
		/** Sources whose texels are linear light (float formats or sRGB views): the output needs gamma encoding. */
		bool IsLinearSource(const FRHITexture* Source) {
			const FRHITextureDesc& Desc = Source->GetDesc();
			if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::SRGB)) {
				return true;
			}
			switch (Desc.Format) {
			case PF_FloatRGBA:
			case PF_FloatRGB:
			case PF_FloatR11G11B10:
			case PF_A32B32G32R32F:
				return true;
			default:
				return false;
			}
		}

		template <typename TPixelShader>
		void DrawScaled(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, FRHITexture* Dest) {
			const FIntPoint DestSize(Dest->GetDesc().Extent.X, Dest->GetDesc().Extent.Y);
			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
			TShaderMapRef<TPixelShader> PixelShader(ShaderMap);

			FRHIRenderPassInfo PassInfo(Dest, ERenderTargetActions::DontLoad_Store);
			RHICmdList.BeginRenderPass(PassInfo, TEXT("IPRtmpPublishCapture"));
			RHICmdList.SetViewport(0, 0, 0.0f, DestSize.X, DestSize.Y, 1.0f);

			FGraphicsPipelineStateInitializer PsoInit;
			RHICmdList.ApplyCachedRenderTargets(PsoInit);
			PsoInit.BlendState = TStaticBlendState<>::GetRHI();
			PsoInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			PsoInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
			PsoInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			PsoInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			PsoInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			PsoInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, PsoInit, 0);
			SetShaderParametersLegacyPS(RHICmdList, PixelShader, TStaticSamplerState<SF_Bilinear>::GetRHI(), Source);

			IRendererModule& Renderer = FModuleManager::GetModuleChecked<IRendererModule>(TEXT("Renderer"));
			Renderer.DrawRectangle(RHICmdList, 0, 0, DestSize.X, DestSize.Y, 0, 0, 1, 1, DestSize, FIntPoint(1, 1), VertexShader, EDRF_Default);
			RHICmdList.EndRenderPass();
		}
	}

	FVideoCapturePool::FVideoCapturePool(int32 InWidth, int32 InHeight) : Width(InWidth), Height(InHeight) {
		for (std::atomic<bool>& Flag : InUse) {
			Flag.store(false);
		}
	}

	FVideoCapturePool::~FVideoCapturePool() = default;

	bool FVideoCapturePool::Capture(FRHICommandListImmediate& RHICmdList, FRHITexture* Source, int64 TimestampMs, ERHIAccess SourceEndState) {
		check(IsInRenderingThread());
		if (!Source) {
			return false;
		}
		int32 Slot = INDEX_NONE;
		for (int32 Index = 0; Index < NumSlots; ++Index) {
			bool bExpected = false;
			if (InUse[Index].compare_exchange_strong(bExpected, true)) {
				Slot = Index;
				break;
			}
		}
		if (Slot == INDEX_NONE) {
			// The encoder is behind: drop this frame rather than queue (latency and memory stay bounded).
			++DroppedFrames;
			return false;
		}
		if (!Textures[Slot].IsValid()) {
			// Shared: the hardware encoder opens it on its own device/queue (same as PixelCapture).
			const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("IPRtmpPublishFrame"), Width, Height, PF_B8G8R8A8)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::Shared)
				.SetClearValue(FClearValueBinding::None)
				.SetInitialState(ERHIAccess::Present);
			Textures[Slot] = RHICmdList.CreateTexture(Desc);
		}
		FRHITexture* Dest = Textures[Slot];

		RHICmdList.Transition(FRHITransitionInfo(Source, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
		RHICmdList.Transition(FRHITransitionInfo(Dest, ERHIAccess::Unknown, ERHIAccess::RTV));
		if (IsLinearSource(Source)) {
			DrawScaled<FScreenPSsRGBSource>(RHICmdList, Source, Dest);
		} else {
			DrawScaled<FScreenPS>(RHICmdList, Source, Dest);
		}
		// Hand both back in the states the rest of the frame / the encoder expect.
		RHICmdList.Transition(FRHITransitionInfo(Source, ERHIAccess::SRVGraphics, SourceEndState));
		RHICmdList.Transition(FRHITransitionInfo(Dest, ERHIAccess::RTV, ERHIAccess::CopySrc));

		FCapturedFrame Frame;
		Frame.Slot = Slot;
		Frame.Texture = Textures[Slot];
		Frame.Fence = RHICreateGPUFence(TEXT("IPRtmpPublishFence"));
		Frame.TimestampMs = TimestampMs;
		Frame.CapturedAt = FPlatformTime::Seconds();
		RHICmdList.WriteGPUFence(Frame.Fence);
		{
			FScopeLock ScopeLock(&QueueLock);
			Queue.Add(MoveTemp(Frame));
		}
		return true;
	}

	bool FVideoCapturePool::TryTake(FCapturedFrame& Out) {
		FScopeLock ScopeLock(&QueueLock);
		while (Queue.Num() > 0) {
			FCapturedFrame& Front = Queue[0];
			if (Front.Fence.IsValid() && Front.Fence->Poll()) {
				Out = MoveTemp(Front);
				Queue.RemoveAt(0, EAllowShrinking::No);
				return true;
			}
			if (FPlatformTime::Seconds() - Front.CapturedAt > 1.0) {
				// GPU never signalled (device lost, renderer stalled): give the slot back.
				InUse[Front.Slot].store(false);
				++DroppedFrames;
				Queue.RemoveAt(0, EAllowShrinking::No);
				continue;
			}
			return false;
		}
		return false;
	}

	void FVideoCapturePool::Release(int32 Slot) {
		if (Slot >= 0 && Slot < NumSlots) {
			InUse[Slot].store(false);
		}
	}
}
