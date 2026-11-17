/**
 * Copyright © 2025 Nathan Miguel
 */

#include "InternetProtocolMedia.h"
#include "IMediaModule.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Player/IPRtmpPipeline.h"
#include "Publish/IPPublishPipeline.h"
#include "Player/IPRtmpPlayerFactory.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPMedia, Log, All);

namespace {
	TUniquePtr<IPMedia::FRtmpPlayerFactory> GPlayerFactory;
}

void FInternetProtocolMediaModule::StartupModule()
{
	// Decoders are created on decode threads, where loading modules is not allowed: load them here.
	if (!FModuleManager::Get().LoadModule(TEXT("ElectraCodecFactory")) || !FModuleManager::Get().LoadModule(TEXT("ElectraDecoders")))
	{
		UE_LOG(LogIPMedia, Error, TEXT("InternetProtocolMedia: Electra decoder modules are not available; RTMP receive will fail."));
	}

	if (IMediaModule* MediaModule = FModuleManager::LoadModulePtr<IMediaModule>(TEXT("Media")))
	{
		GPlayerFactory = MakeUnique<IPMedia::FRtmpPlayerFactory>();
		MediaModule->RegisterPlayerFactory(*GPlayerFactory);
	}
}

void FInternetProtocolMediaModule::ShutdownModule()
{
	if (GPlayerFactory.IsValid())
	{
		if (IMediaModule* MediaModule = FModuleManager::GetModulePtr<IMediaModule>(TEXT("Media")))
		{
			MediaModule->UnregisterPlayerFactory(*GPlayerFactory);
		}
		GPlayerFactory.Reset();
	}

	// Decode/encode threads stop on their own once their player/publisher is closed; at engine exit give them a bounded
	// moment so they do not run while the module is unloaded.
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	auto Running = []() { return IPMedia::FRtmpPipeline::GetRunningThreads() + IPMedia::FPublishPipeline::GetRunningThreads(); };
	while (Running() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.005f);
	}
	if (Running() > 0)
	{
		UE_LOG(LogIPMedia, Warning, TEXT("InternetProtocolMedia: %d decode/encode thread(s) still running at shutdown."), Running());
	}
}

IMPLEMENT_MODULE(FInternetProtocolMediaModule, InternetProtocolMedia)
