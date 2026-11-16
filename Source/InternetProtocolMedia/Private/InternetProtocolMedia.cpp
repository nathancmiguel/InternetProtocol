/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "InternetProtocolMedia.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPMedia, Log, All);

void FInternetProtocolMediaModule::StartupModule()
{
	// Decoders are created on decode threads, where loading modules is not allowed: load them here.
	if (!FModuleManager::Get().LoadModule(TEXT("ElectraCodecFactory")) || !FModuleManager::Get().LoadModule(TEXT("ElectraDecoders")))
	{
		UE_LOG(LogIPMedia, Error, TEXT("InternetProtocolMedia: Electra decoder modules are not available; RTMP receive will fail."));
	}
}

void FInternetProtocolMediaModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInternetProtocolMediaModule, InternetProtocolMedia)
