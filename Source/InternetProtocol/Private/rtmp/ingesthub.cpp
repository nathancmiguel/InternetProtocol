/**
 * Copyright © 2025 Nathan Miguel
 */

#include "rtmp/rtmpmedia.h"
#include "rtmp/rtmpserversession.h"
#include "Misc/ScopeLock.h"

/*
 * FRtmpIngestHub: process-wide registry connecting RTMP publishers (server sessions) with media sinks.
 * The lock is never held while calling into a session or a sink (sessions are only *posted* to), so there
 * is no lock-order risk with the network strands.
 */
namespace IPNet::Rtmp {
	namespace {
		struct FSinkEntry {
			FString Key;
			TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink;
		};

		struct FPortEntry {
			std::weak_ptr<FRtmpServerCore> Server;
			/** Internal listener owned by the hub (null when the port is served by a URTMPServer). */
			std::shared_ptr<FRtmpServerCore> Owned;
			int32 References = 0;
		};

		struct FHubState {
			FCriticalSection Lock;
			int64 NextHandle = 1;
			TMap<int64, FSinkEntry> Sinks;
			TMap<FString, std::weak_ptr<FRtmpServerSession>> Publishers;
			TMap<uint16, FPortEntry> Ports;
			TMap<int64, uint16> PortHandles;
		};

		FHubState& State() {
			static FHubState Instance;
			return Instance;
		}
	}

	namespace Hub {
		FString MakeKey(uint16 Port, const FString& App, const FString& StreamName) {
			return FString::Printf(TEXT("%u|%s|%s"), Port, *App, *StreamName);
		}

		bool BeginPublish(const FString& Key, const std::shared_ptr<FRtmpServerSession>& Session) {
			TArray<TPair<int64, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe>>> ToAttach;
			{
				FHubState& S = State();
				FScopeLock ScopeLock(&S.Lock);
				if (std::weak_ptr<FRtmpServerSession>* Existing = S.Publishers.Find(Key)) {
					if (!Existing->expired()) {
						return false;
					}
				}
				S.Publishers.Add(Key, Session);
				for (const TPair<int64, FSinkEntry>& Pair : S.Sinks) {
					if (Pair.Value.Key.Equals(Key, ESearchCase::CaseSensitive)) {
						ToAttach.Emplace(Pair.Key, Pair.Value.Sink);
					}
				}
			}
			// Attach is posted to the session strand; it runs after the publish handler returns, so sinks see
			// OnStreamStarted before any media.
			for (const TPair<int64, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe>>& Pair : ToAttach) {
				Session->AttachSink(Pair.Key, Pair.Value);
			}
			return true;
		}

		void EndPublish(const FString& Key, const FRtmpServerSession* Session) {
			FHubState& S = State();
			FScopeLock ScopeLock(&S.Lock);
			if (std::weak_ptr<FRtmpServerSession>* Existing = S.Publishers.Find(Key)) {
				std::shared_ptr<FRtmpServerSession> Strong = Existing->lock();
				if (!Strong || Strong.get() == Session) {
					S.Publishers.Remove(Key);
				}
			}
		}

		void RegisterServer(uint16 Port, const std::shared_ptr<FRtmpServerCore>& Server) {
			FHubState& S = State();
			FScopeLock ScopeLock(&S.Lock);
			FPortEntry& Entry = S.Ports.FindOrAdd(Port);
			Entry.Server = Server;
		}

		void UnregisterServer(uint16 Port, const FRtmpServerCore* Server) {
			FHubState& S = State();
			FScopeLock ScopeLock(&S.Lock);
			if (FPortEntry* Entry = S.Ports.Find(Port)) {
				std::shared_ptr<FRtmpServerCore> Strong = Entry->Server.lock();
				if (!Strong || Strong.get() == Server) {
					Entry->Server.reset();
					if (Entry->References == 0) {
						S.Ports.Remove(Port);
					}
				}
			}
		}
	}
}

using namespace IPNet::Rtmp;

int64 FRtmpIngestHub::RegisterSink(uint16 Port, const FString& App, const FString& StreamName, TSharedRef<IRtmpMediaSink, ESPMode::ThreadSafe> Sink) {
	const FString Key = Hub::MakeKey(Port, App, StreamName);
	std::shared_ptr<FRtmpServerSession> Live;
	int64 Handle;
	{
		FHubState& S = State();
		FScopeLock ScopeLock(&S.Lock);
		Handle = S.NextHandle++;
		S.Sinks.Add(Handle, FSinkEntry{Key, Sink});
		if (std::weak_ptr<FRtmpServerSession>* Publisher = S.Publishers.Find(Key)) {
			Live = Publisher->lock();
		}
	}
	if (Live) {
		Live->AttachSink(Handle, Sink);
	}
	return Handle;
}

void FRtmpIngestHub::UnregisterSink(int64 Handle) {
	std::shared_ptr<FRtmpServerSession> Live;
	{
		FHubState& S = State();
		FScopeLock ScopeLock(&S.Lock);
		const FSinkEntry* Entry = S.Sinks.Find(Handle);
		if (!Entry) {
			return;
		}
		if (std::weak_ptr<FRtmpServerSession>* Publisher = S.Publishers.Find(Entry->Key)) {
			Live = Publisher->lock();
		}
		S.Sinks.Remove(Handle);
	}
	// The sink may still receive a few calls already queued on the strand; it is kept alive by its TSharedRef.
	if (Live) {
		Live->DetachSink(Handle);
	}
}

int64 FRtmpIngestHub::AcquirePort(uint16 Port, FString& OutError) {
	FHubState& S = State();
	{
		FScopeLock ScopeLock(&S.Lock);
		if (FPortEntry* Entry = S.Ports.Find(Port)) {
			if (!Entry->Server.expired() || Entry->Owned) {
				++Entry->References;
				const int64 Handle = S.NextHandle++;
				S.PortHandles.Add(Handle, Port);
				return Handle;
			}
		}
	}

	// Nothing serves this port: open an internal listener on all interfaces.
	std::shared_ptr<FRtmpServerCore> Server = FRtmpServerCore::Create(FRtmpServerCallbacks());
	asio::error_code Ec;
	// No SO_REUSEADDR: on Windows it would let two listeners share the port silently.
	if (!Server->Open(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), Port), false, 0, Ec)) {
		OutError = FString::Printf(TEXT("could not listen on RTMP port %u: %hs"), Port, Ec.message().c_str());
		return 0;
	}
	FScopeLock ScopeLock(&S.Lock);
	FPortEntry& Entry = S.Ports.FindOrAdd(Port);
	Entry.Owned = Server;
	Entry.Server = Server;
	++Entry.References;
	const int64 Handle = S.NextHandle++;
	S.PortHandles.Add(Handle, Port);
	return Handle;
}

void FRtmpIngestHub::ReleasePort(int64 Handle) {
	std::shared_ptr<FRtmpServerCore> ToClose;
	{
		FHubState& S = State();
		FScopeLock ScopeLock(&S.Lock);
		uint16 Port;
		if (!S.PortHandles.RemoveAndCopyValue(Handle, Port)) {
			return;
		}
		FPortEntry* Entry = S.Ports.Find(Port);
		if (!Entry) {
			return;
		}
		if (--Entry->References <= 0) {
			Entry->References = 0;
			ToClose = MoveTemp(Entry->Owned);
			Entry->Owned.reset();
			if (Entry->Server.expired() || ToClose) {
				S.Ports.Remove(Port);
			}
		}
	}
	if (ToClose) {
		ToClose->Close();
	}
}
