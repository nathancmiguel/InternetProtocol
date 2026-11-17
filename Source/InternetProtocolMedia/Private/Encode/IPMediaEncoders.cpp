/**
 * Copyright © 2025 Nathan Miguel
 */

#include "IPMediaEncoders.h"
#include "Encode/IPEncoderDefaults.h"
#include "Misc/ScopeLock.h"

namespace {
	FCriticalSection EncoderFactoryLock;

	FIPVideoEncoderFactory& VideoEncoderFactory() {
		static FIPVideoEncoderFactory Factory;
		return Factory;
	}

	FIPAudioEncoderFactory& AudioEncoderFactory() {
		static FIPAudioEncoderFactory Factory;
		return Factory;
	}
}

void FIPMediaEncoders::SetVideoEncoderFactory(FIPVideoEncoderFactory Factory) {
	FScopeLock ScopeLock(&EncoderFactoryLock);
	VideoEncoderFactory() = MoveTemp(Factory);
}

void FIPMediaEncoders::SetAudioEncoderFactory(FIPAudioEncoderFactory Factory) {
	FScopeLock ScopeLock(&EncoderFactoryLock);
	AudioEncoderFactory() = MoveTemp(Factory);
}

TSharedPtr<IIPVideoEncoder, ESPMode::ThreadSafe> FIPMediaEncoders::CreateVideoEncoder() {
	FIPVideoEncoderFactory Factory;
	{
		FScopeLock ScopeLock(&EncoderFactoryLock);
		Factory = VideoEncoderFactory();
	}
	return Factory ? Factory() : IPMedia::CreateDefaultVideoEncoder();
}

TSharedPtr<IIPAudioEncoder, ESPMode::ThreadSafe> FIPMediaEncoders::CreateAudioEncoder() {
	FIPAudioEncoderFactory Factory;
	{
		FScopeLock ScopeLock(&EncoderFactoryLock);
		Factory = AudioEncoderFactory();
	}
	return Factory ? Factory() : IPMedia::CreateDefaultAudioEncoder();
}
