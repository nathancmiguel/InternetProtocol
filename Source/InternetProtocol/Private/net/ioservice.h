/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "net/common.h"
#include "HAL/CriticalSection.h"
#include <atomic>
#include <memory>

/*
 * Threading architecture
 * ----------------------
 * The module owns ONE asio::io_context, run by a small fixed set of FRunnableThreads (FIoService).
 * Every connection / socket is a native object ("session") whose I/O objects are bound to their own
 * strand, so the handlers of one session never run concurrently while different sessions run in parallel.
 *
 *   UObject facade (Game Thread only)
 *        |  std::shared_ptr               ^  AsyncTask(GameThread) + TWeakObjectPtr<UObject>
 *        v                                 |
 *   native session (strand) ---------------+
 *        |  std::shared_ptr<FIoCore>
 *        v
 *   FIoCore (io_context + registry)  <---  FIoService worker threads
 *
 * Rules:
 *  - Native sessions never touch UObjects. They only post results to the Game Thread capturing a
 *    TWeakObjectPtr, which is checked before use.
 *  - Asio handlers capture a shared_ptr to the session (never a bare `this`), so the session outlives every
 *    pending operation. Destroying the UObject only requests a close and drops its reference.
 *  - Every session holds a shared_ptr to FIoCore, so the io_context outlives every socket bound to it.
 *  - The Game Thread never blocks on network work. The only blocking join happens in
 *    FInternetProtocolModule::ShutdownModule (engine exit / module unload), bounded by a timeout.
 *  - Hot reload: only Live Coding is supported. Unloading the module DLL while UObjects of this plugin are
 *    alive is not supported (their code would be unloaded under them).
 */
namespace IPNet {

	/** Anything the service must be able to abort at shutdown. */
	class FNetObject {
	public:
		virtual ~FNetObject() = default;
		/** Immediately closes the object. Thread-safe, idempotent, non-blocking. */
		virtual void abort() = 0;
	};

	class FIoCore {
	public:
		asio::io_context Context;

		bool IsRunning() const { return bRunning.load(std::memory_order_acquire); }

		void Register(const std::shared_ptr<FNetObject>& Object);
		void Unregister(const FNetObject* Object);
		TArray<std::shared_ptr<FNetObject>> Snapshot();

	private:
		friend class FIoService;
		std::atomic<bool> bRunning{true};
		FCriticalSection Lock;
		TMap<const FNetObject*, std::weak_ptr<FNetObject>> Objects;
	};

	class FIoService {
	public:
		/** Starts the worker threads. Called from StartupModule. */
		static void Startup();

		/** Aborts every live session and joins the worker threads. Called from ShutdownModule. */
		static void Shutdown();

		/** The running core, or nullptr when the service is not running (before startup / after shutdown). */
		static std::shared_ptr<FIoCore> Get();
	};
}
