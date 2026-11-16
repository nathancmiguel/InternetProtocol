/** 
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"

namespace IPMedia {
	/** iprtmp://host:port/app/key -> (port, app, key). The host is informational (the listener binds all interfaces). */
	inline bool ParseIpRtmpUrl(const FString& Url, uint16& OutPort, FString& OutApp, FString& OutKey, FString& OutError) {
		static const FString Scheme = TEXT("iprtmp://");
		if (!Url.StartsWith(Scheme, ESearchCase::IgnoreCase)) {
			OutError = TEXT("URL must start with iprtmp://");
			return false;
		}
		FString HostPort, Path;
		if (!Url.Mid(Scheme.Len()).Split(TEXT("/"), &HostPort, &Path)) {
			OutError = TEXT("URL must be iprtmp://host:port/app/key");
			return false;
		}
		FString Host, Port = TEXT("1935");
		if (!HostPort.Split(TEXT(":"), &Host, &Port, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
			Host = HostPort;
		}
		const int32 PortNumber = Port.IsNumeric() ? FCString::Atoi(*Port) : -1;
		if (PortNumber <= 0 || PortNumber > 65535) {
			OutError = TEXT("invalid port");
			return false;
		}
		Path.RemoveFromEnd(TEXT("/"));
		if (!Path.Split(TEXT("/"), &OutApp, &OutKey, ESearchCase::IgnoreCase, ESearchDir::FromEnd) || OutApp.IsEmpty() || OutKey.IsEmpty()) {
			OutError = TEXT("URL must contain an application and a stream key (iprtmp://host:port/app/key)");
			return false;
		}
		int32 Query;
		if (OutKey.FindChar(TEXT('?'), Query)) {
			OutKey.LeftInline(Query);
		}
		OutPort = static_cast<uint16>(PortNumber);
		return true;
	}

	/** URL safe to log: the stream key is replaced by ***. */
	inline FString MaskIpRtmpUrl(const FString& Url) {
		int32 Slash;
		return Url.FindLastChar(TEXT('/'), Slash) && Slash > 9 ? Url.Left(Slash + 1) + TEXT("***") : FString(TEXT("iprtmp://***"));
	}
}
