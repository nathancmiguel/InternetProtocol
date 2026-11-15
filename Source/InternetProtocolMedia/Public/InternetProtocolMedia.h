/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "Modules/ModuleInterface.h"

/**
 * InternetProtocolMedia: receives RTMP audio/video (iprtmp:// media player on top of the Electra decoders)
 * and publishes Unreal audio/video over RTMP. Kept separate so the InternetProtocol networking module
 * never depends on media, codec or video RHI modules.
 */
class FInternetProtocolMediaModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
