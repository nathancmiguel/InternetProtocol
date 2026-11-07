/**
 * Copyright © 2025 Nathan Miguel
 */

#include "InternetProtocol.h"
#include "Modules/ModuleManager.h"
#include "net/common.h"
#include "net/ioservice.h"
#include "net/tls.h"

DEFINE_LOG_CATEGORY(LogAsio);

#define LOCTEXT_NAMESPACE "FInternetProtocolModule"

void FInternetProtocolModule::StartupModule()
{
	IPNet::FIoService::Startup();
}

void FInternetProtocolModule::ShutdownModule()
{
	// Aborts every live connection and joins the I/O threads (bounded). UObjects that still exist keep
	// their native sessions alive, but no handler runs anymore and no callback reaches the Game Thread.
	IPNet::FIoService::Shutdown();
	IPNet::ShutdownTls();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FInternetProtocolModule, InternetProtocol)
