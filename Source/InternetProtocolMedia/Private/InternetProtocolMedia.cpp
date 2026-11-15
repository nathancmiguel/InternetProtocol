/** 
 * Copyright © 2025 Nathan Miguel
 */

#include "InternetProtocolMedia.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogIPMedia, Log, All);

void FInternetProtocolMediaModule::StartupModule()
{
	// The iprtmp:// media player factory is registered here once it is implemented.
}

void FInternetProtocolMediaModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInternetProtocolMediaModule, InternetProtocolMedia)
