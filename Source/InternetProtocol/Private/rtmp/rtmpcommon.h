/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include <string>

/*
 * Helpers shared by the RTMP server session and the RTMP clients (publish and play).
 */
namespace IPNet::Rtmp {

	struct FParsedUrl {
		bool bTls = false;
		std::string Host;
		std::string Port;
		std::string App;
		std::string StreamName;
		std::string TcUrl;
	};

	inline bool ParseRtmpUrl(const FString& Url, const FString& StreamKey, FParsedUrl& Out, FString& OutError) {
		FString Rest;
		if (Url.StartsWith(TEXT("rtmps://"), ESearchCase::IgnoreCase)) {
			Out.bTls = true;
			Rest = Url.Mid(8);
		} else if (Url.StartsWith(TEXT("rtmp://"), ESearchCase::IgnoreCase)) {
			Rest = Url.Mid(7);
		} else {
			OutError = TEXT("URL must start with rtmp:// or rtmps://");
			return false;
		}
		FString HostPort, Path;
		if (!Rest.Split(TEXT("/"), &HostPort, &Path)) {
			OutError = TEXT("URL has no application path (rtmp://host/app)");
			return false;
		}
		FString Host = HostPort, Port = Out.bTls ? TEXT("443") : TEXT("1935");
		if (HostPort.StartsWith(TEXT("["))) {
			int32 Close;
			if (!HostPort.FindChar(TEXT(']'), Close)) {
				OutError = TEXT("invalid IPv6 host");
				return false;
			}
			Host = HostPort.Mid(1, Close - 1);
			if (HostPort.Len() > Close + 2 && HostPort[Close + 1] == TEXT(':')) {
				Port = HostPort.Mid(Close + 2);
			}
		} else {
			FString Left, Right;
			if (HostPort.Split(TEXT(":"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
				Host = Left;
				Port = Right;
			}
		}
		if (Host.IsEmpty() || !Port.IsNumeric() || FCString::Atoi(*Port) <= 0 || FCString::Atoi(*Port) > 65535) {
			OutError = TEXT("invalid host or port");
			return false;
		}
		while (Path.EndsWith(TEXT("/"))) {
			Path.LeftChopInline(1);
		}
		FString App = Path, Stream = StreamKey;
		if (Stream.IsEmpty()) {
			FString Left, Right;
			if (!Path.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
				OutError = TEXT("no stream key: pass it separately or as rtmp://host/app/KEY");
				return false;
			}
			App = Left;
			Stream = Right;
		}
		if (App.IsEmpty() || Stream.IsEmpty()) {
			OutError = TEXT("empty application or stream key");
			return false;
		}
		Out.Host = TCHAR_TO_UTF8(*Host);
		Out.Port = TCHAR_TO_UTF8(*Port);
		Out.App = TCHAR_TO_UTF8(*App);
		Out.StreamName = TCHAR_TO_UTF8(*Stream);
		const FString TcUrl = FString::Printf(TEXT("%s://%s:%s/%s"), Out.bTls ? TEXT("rtmps") : TEXT("rtmp"),
			Host.Contains(TEXT(":")) ? *FString::Printf(TEXT("[%s]"), *Host) : *Host, *Port, *App);
		Out.TcUrl = TCHAR_TO_UTF8(*TcUrl);
		return true;
	}

	inline FString ToFString(const std::string& Text) {
		return FString(UTF8_TO_TCHAR(Text.c_str()));
	}

	/** Extends 32-bit RTMP timestamps to 64 bits across the ~49.7-day wrap. */
	struct FTimestampUnwrapper {
		uint32 Last = 0;
		int64 Wraps = 0;
		bool bStarted = false;

		int64 Unwrap(uint32 Timestamp) {
			if (bStarted && Timestamp < Last && (Last - Timestamp) > 0x80000000u) {
				++Wraps;
			}
			bStarted = true;
			Last = Timestamp;
			return (Wraps << 32) | Timestamp;
		}

		void Reset() { *this = FTimestampUnwrapper(); }
	};
}
