/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IMediaPlayerFactory.h"

namespace IPMedia {
	/** Registers the iprtmp:// scheme with the Media Framework. */
	class FRtmpPlayerFactory final : public IMediaPlayerFactory {
	public:
		virtual bool CanPlayUrl(const FString& Url, const IMediaOptions* Options, TArray<FText>* OutWarnings, TArray<FText>* OutErrors) const override;
		virtual TSharedPtr<IMediaPlayer, ESPMode::ThreadSafe> CreatePlayer(IMediaEventSink& EventSink) override;
		virtual FText GetDisplayName() const override;
		virtual FName GetPlayerName() const override;
		virtual FGuid GetPlayerPluginGUID() const override;
		virtual const TArray<FString>& GetSupportedPlatforms() const override;
		virtual bool SupportsFeature(EMediaFeature Feature) const override;
	};
}
