/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "IMediaCache.h"
#include "IMediaControls.h"
#include "IMediaPlayer.h"
#include "IMediaTracks.h"
#include "IMediaView.h"
#include "IPMediaTypes.h"

class FMediaSamples;
class IMediaEventSink;

namespace IPMedia {
	class FRtmpPipeline;

	/** Identifies this player (also used by UIPRtmpReceiver to reach its statistics). */
	extern const FGuid RtmpPlayerPluginGuid;

	/**
	 * IMediaPlayer for iprtmp:// URLs. Opening succeeds as soon as the port is listened on: the player then
	 * waits for a publisher (MediaBuffering) and starts presenting once the jitter buffer is filled. The
	 * player is driven by the Media Framework on the Game Thread; decoding happens in FRtmpPipeline.
	 */
	class FRtmpMediaPlayer final : public IMediaPlayer, protected IMediaCache, protected IMediaControls, protected IMediaTracks, protected IMediaView {
	public:
		explicit FRtmpMediaPlayer(IMediaEventSink& InEventSink);
		virtual ~FRtmpMediaPlayer() override;

		// IMediaPlayer
		virtual void Close() override;
		virtual IMediaCache& GetCache() override { return *this; }
		virtual IMediaControls& GetControls() override { return *this; }
		virtual FString GetInfo() const override;
		virtual FGuid GetPlayerPluginGUID() const override { return RtmpPlayerPluginGuid; }
		virtual IMediaSamples& GetSamples() override;
		virtual FString GetStats() const override;
		virtual IMediaTracks& GetTracks() override { return *this; }
		virtual FString GetUrl() const override { return Url; }
		virtual IMediaView& GetView() override { return *this; }
		virtual bool Open(const FString& InUrl, const IMediaOptions* Options) override;
		virtual bool Open(const TSharedRef<FArchive, ESPMode::ThreadSafe>& Archive, const FString& OriginalUrl, const IMediaOptions* Options) override { return false; }
		virtual void TickInput(FTimespan DeltaTime, FTimespan Timecode) override;
		virtual bool GetPlayerFeatureFlag(EFeatureFlag Flag) const override;

		/** Game Thread: statistics and last presented frame for UIPRtmpReceiver. */
		FIPRtmpReceiverStats GetReceiverStats() const;
		FIPVideoFrameInfo GetLastFrame(uint64& OutFrameCounter) const;
		bool TakeError(FString& OutError);

	protected:
		// IMediaControls
		virtual bool CanControl(EMediaControl Control) const override;
		virtual FTimespan GetDuration() const override { return FTimespan::MaxValue(); }
		virtual float GetRate() const override { return Rate; }
		virtual EMediaState GetState() const override { return State; }
		virtual EMediaStatus GetStatus() const override;
		virtual TRangeSet<float> GetSupportedRates(EMediaRateThinning Thinning) const override;
		virtual FTimespan GetTime() const override;
		virtual bool IsLooping() const override { return false; }
		virtual bool Seek(const FTimespan& Time) override { return false; }
		virtual bool SetLooping(bool Looping) override { return false; }
		virtual bool SetRate(float InRate) override;

		// IMediaTracks
		virtual bool GetAudioTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaAudioTrackFormat& OutFormat) const override;
		virtual int32 GetNumTracks(EMediaTrackType TrackType) const override;
		virtual int32 GetNumTrackFormats(EMediaTrackType TrackType, int32 TrackIndex) const override;
		virtual int32 GetSelectedTrack(EMediaTrackType TrackType) const override;
		virtual FText GetTrackDisplayName(EMediaTrackType TrackType, int32 TrackIndex) const override;
		virtual int32 GetTrackFormat(EMediaTrackType TrackType, int32 TrackIndex) const override;
		virtual FString GetTrackLanguage(EMediaTrackType TrackType, int32 TrackIndex) const override { return FString(); }
		virtual FString GetTrackName(EMediaTrackType TrackType, int32 TrackIndex) const override;
		virtual bool GetVideoTrackFormat(int32 TrackIndex, int32 FormatIndex, FMediaVideoTrackFormat& OutFormat) const override;
		virtual bool SelectTrack(EMediaTrackType TrackType, int32 TrackIndex) override;
		virtual bool SetTrackFormat(EMediaTrackType TrackType, int32 TrackIndex, int32 FormatIndex) override { return TrackIndex == 0 && FormatIndex == 0; }

	private:
		IMediaEventSink& EventSink;
		TSharedPtr<FMediaSamples, ESPMode::ThreadSafe> Samples;
		TSharedPtr<FRtmpPipeline, ESPMode::ThreadSafe> Pipeline;
		FString Url;
		EMediaState State = EMediaState::Closed;
		float Rate = 0.0f;
		bool bBuffering = false;
		FIPRtmpReceiverStats CachedStats;
	};
}
