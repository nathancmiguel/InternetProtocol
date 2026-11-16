/**
 * Copyright © 2025 Nathan Miguel
 */

#include "Player/IPRtmpPlayerFactory.h"
#include "Player/IPRtmpMediaPlayer.h"
#include "Player/IPRtmpUrl.h"
#include "IPRtmpMediaSource.h"

#define LOCTEXT_NAMESPACE "IPRtmpPlayerFactory"

namespace IPMedia {
	bool FRtmpPlayerFactory::CanPlayUrl(const FString& Url, const IMediaOptions* Options, TArray<FText>* OutWarnings, TArray<FText>* OutErrors) const {
		if (!Url.StartsWith(TEXT("iprtmp://"), ESearchCase::IgnoreCase)) {
			return false;
		}
		uint16 Port;
		FString App, Key, Error;
		if (!ParseIpRtmpUrl(Url, Port, App, Key, Error)) {
			if (OutErrors) {
				OutErrors->Add(FText::FromString(Error));
			}
			return false;
		}
		return true;
	}

	TSharedPtr<IMediaPlayer, ESPMode::ThreadSafe> FRtmpPlayerFactory::CreatePlayer(IMediaEventSink& EventSink) {
		return MakeShared<FRtmpMediaPlayer, ESPMode::ThreadSafe>(EventSink);
	}

	FText FRtmpPlayerFactory::GetDisplayName() const {
		return LOCTEXT("DisplayName", "InternetProtocol RTMP");
	}

	FName FRtmpPlayerFactory::GetPlayerName() const {
		static const FName Name(TEXT("InternetProtocolRtmp"));
		return Name;
	}

	FGuid FRtmpPlayerFactory::GetPlayerPluginGUID() const {
		return RtmpPlayerPluginGuid;
	}

	const TArray<FString>& FRtmpPlayerFactory::GetSupportedPlatforms() const {
		static const TArray<FString> Platforms = {TEXT("Windows")};
		return Platforms;
	}

	bool FRtmpPlayerFactory::SupportsFeature(EMediaFeature Feature) const {
		return Feature == EMediaFeature::AudioSamples || Feature == EMediaFeature::AudioTracks || Feature == EMediaFeature::VideoSamples ||
		       Feature == EMediaFeature::VideoTracks;
	}
}

// -------------------------------------------------------------------------------------------------------------
// UIPRtmpMediaSource

const FName UIPRtmpMediaSource::LatencyModeOption(TEXT("IPRtmp.LatencyMode"));
const FName UIPRtmpMediaSource::LatencyMsOption(TEXT("IPRtmp.LatencyMs"));

bool UIPRtmpMediaSource::Validate() const {
	uint16 Port;
	FString App, Key, Error;
	return IPMedia::ParseIpRtmpUrl(StreamUrl, Port, App, Key, Error);
}

bool UIPRtmpMediaSource::GetMediaOption(const FName& Key, bool DefaultValue) const {
	return Super::GetMediaOption(Key, DefaultValue);
}

int64 UIPRtmpMediaSource::GetMediaOption(const FName& Key, int64 DefaultValue) const {
	if (Key == LatencyModeOption) {
		return static_cast<int64>(LatencyMode);
	}
	if (Key == LatencyMsOption) {
		return LatencyMs;
	}
	return Super::GetMediaOption(Key, DefaultValue);
}

FString UIPRtmpMediaSource::GetMediaOption(const FName& Key, const FString& DefaultValue) const {
	return Super::GetMediaOption(Key, DefaultValue);
}

double UIPRtmpMediaSource::GetMediaOption(const FName& Key, double DefaultValue) const {
	return Super::GetMediaOption(Key, DefaultValue);
}

FText UIPRtmpMediaSource::GetMediaOption(const FName& Key, const FText& DefaultValue) const {
	return Super::GetMediaOption(Key, DefaultValue);
}

bool UIPRtmpMediaSource::HasMediaOption(const FName& Key) const {
	return Key == LatencyModeOption || Key == LatencyMsOption || Super::HasMediaOption(Key);
}

#undef LOCTEXT_NAMESPACE
