/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace IPNet {
	/**
	 * Runs Fn(Object) on the Game Thread if the object is still alive when the task executes.
	 * Only a TWeakObjectPtr is captured, never a raw pointer, so a UObject destroyed while results are in
	 * flight is never touched. Captured data must be owned by value.
	 */
	template <typename T, typename FFunc>
	void RunOnGameThread(const TWeakObjectPtr<T>& Weak, FFunc&& Fn) {
		AsyncTask(ENamedThreads::GameThread, [Weak, Fn = Forward<FFunc>(Fn)]() mutable {
			if (T* Object = Weak.Get()) {
				Fn(Object);
			}
		});
	}
}
