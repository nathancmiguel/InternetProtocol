/**
 * Copyright © 2025 Nathan Miguel
 */

#include "net/ioservice.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace IPNet {

	void FIoCore::Register(const std::shared_ptr<FNetObject>& Object) {
		FScopeLock ScopeLock(&Lock);
		Objects.Add(Object.get(), Object);
	}

	void FIoCore::Unregister(const FNetObject* Object) {
		FScopeLock ScopeLock(&Lock);
		Objects.Remove(Object);
	}

	TArray<std::shared_ptr<FNetObject>> FIoCore::Snapshot() {
		TArray<std::shared_ptr<FNetObject>> Result;
		FScopeLock ScopeLock(&Lock);
		Result.Reserve(Objects.Num());
		for (const TPair<const FNetObject*, std::weak_ptr<FNetObject>>& Pair : Objects) {
			if (std::shared_ptr<FNetObject> Strong = Pair.Value.lock()) {
				Result.Add(MoveTemp(Strong));
			}
		}
		return Result;
	}

	namespace {
		class FIoWorker final : public FRunnable {
		public:
			FIoWorker(std::shared_ptr<FIoCore> InCore, std::atomic<int32>& InActive)
				: Core(MoveTemp(InCore)), Active(InActive) {}

			virtual uint32 Run() override {
				Core->Context.run();
				Active.fetch_sub(1, std::memory_order_acq_rel);
				return 0;
			}

		private:
			std::shared_ptr<FIoCore> Core;
			std::atomic<int32>& Active;
		};

		using FWorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

		struct FServiceState {
			FCriticalSection Lock;
			std::shared_ptr<FIoCore> Core;
			TUniquePtr<FWorkGuard> WorkGuard;
			TArray<TUniquePtr<FIoWorker>> Workers;
			TArray<FRunnableThread*> Threads;
			std::atomic<int32> Active{0};
		};

		FServiceState& State() {
			static FServiceState Instance;
			return Instance;
		}
	}

	void FIoService::Startup() {
		FServiceState& S = State();
		FScopeLock ScopeLock(&S.Lock);
		if (S.Core) {
			return;
		}

		if (!FPlatformProcess::SupportsMultithreading()) {
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol requires multithreading support; networking is disabled."));
			return;
		}

		S.Core = std::make_shared<FIoCore>();
		S.WorkGuard = MakeUnique<FWorkGuard>(asio::make_work_guard(S.Core->Context));

		const int32 NumThreads = FMath::Clamp(FPlatformMisc::NumberOfCores() / 2, 1, 4);
		for (int32 Index = 0; Index < NumThreads; ++Index) {
			S.Active.fetch_add(1, std::memory_order_acq_rel);
			TUniquePtr<FIoWorker> Worker = MakeUnique<FIoWorker>(S.Core, S.Active);
			FRunnableThread* Thread = FRunnableThread::Create(Worker.Get(),
				*FString::Printf(TEXT("InternetProtocol.IO.%d"), Index), 0, TPri_Normal);
			if (!Thread) {
				S.Active.fetch_sub(1, std::memory_order_acq_rel);
				UE_LOG(LogAsio, Error, TEXT("InternetProtocol: failed to create I/O thread %d"), Index);
				continue;
			}
			S.Workers.Add(MoveTemp(Worker));
			S.Threads.Add(Thread);
		}

		if (S.Threads.Num() == 0) {
			S.WorkGuard.Reset();
			S.Core.reset();
			UE_LOG(LogAsio, Error, TEXT("InternetProtocol: no I/O thread could be created; networking is disabled."));
			return;
		}

		UE_LOG(LogAsio, Log, TEXT("InternetProtocol: started %d I/O thread(s)."), S.Threads.Num());
	}

	void FIoService::Shutdown() {
		FServiceState& S = State();
		std::shared_ptr<FIoCore> Core;
		TArray<FRunnableThread*> Threads;
		TArray<TUniquePtr<FIoWorker>> Workers;
		{
			FScopeLock ScopeLock(&S.Lock);
			if (!S.Core) {
				return;
			}
			Core = MoveTemp(S.Core);
			S.Core.reset();
			Threads = MoveTemp(S.Threads);
			Workers = MoveTemp(S.Workers);
		}

		// 1. Refuse new sessions and abort every live one. Aborts are posted to each session strand.
		Core->bRunning.store(false, std::memory_order_release);
		for (const std::shared_ptr<FNetObject>& Object : Core->Snapshot()) {
			Object->abort();
		}

		// 2. Let the workers drain the queue (run() returns once there is no more work), bounded in time:
		// a pending DNS lookup cannot be interrupted and must not hang the editor on exit.
		S.WorkGuard.Reset();
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (S.Active.load(std::memory_order_acquire) > 0 && FPlatformTime::Seconds() < Deadline) {
			FPlatformProcess::SleepNoStats(0.005f);
		}
		Core->Context.stop();

		for (FRunnableThread* Thread : Threads) {
			Thread->WaitForCompletion();
			delete Thread;
		}
		Workers.Empty();

		// 3. Run, on this thread, the completion handlers that are still queued (aborted operations) so the
		// sessions they reference are released now instead of staying alive inside the stopped io_context.
		Core->Context.restart();
		for (int32 Iteration = 0; Iteration < 50; ++Iteration) {
			if (Core->Context.poll() == 0) {
				FPlatformProcess::SleepNoStats(0.002f);
				if (Core->Context.poll() == 0) {
					break;
				}
			}
		}

		UE_LOG(LogAsio, Log, TEXT("InternetProtocol: I/O service stopped."));
		// Core is released here; sessions still owned by UObjects keep it alive until they are destroyed.
	}

	std::shared_ptr<FIoCore> FIoService::Get() {
		FServiceState& S = State();
		FScopeLock ScopeLock(&S.Lock);
		return S.Core;
	}
}
