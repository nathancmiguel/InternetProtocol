/**
 * Copyright © 2025 Nathan Miguel
 */

#include "InternetProtocolMedia.h"
#include "IMediaModule.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Player/IPRtmpPipeline.h"
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

	// Decode threads stop on their own once their player is closed; at engine exit give them a bounded
	// moment so they do not run while the module is unloaded.
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (IPMedia::FRtmpPipeline::GetRunningThreads() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.005f);
	}
	if (IPMedia::FRtmpPipeline::GetRunningThreads() > 0)
	{
		UE_LOG(LogIPMedia, Warning, TEXT("InternetProtocolMedia: %d decode thread(s) still running at shutdown."), IPMedia::FRtmpPipeline::GetRunningThreads());
	}
}

IMPLEMENT_MODULE(FInternetProtocolMediaModule, InternetProtocolMedia)
