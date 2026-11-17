/**
 * Copyright © 2025 Nathan Miguel
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "net/fwd.h"
#include "rtmpserver.generated.h"

namespace IPNet::Rtmp {
	class FRtmpServerCore;
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDelegateRtmpServer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FDelegateRtmpServerPublish, const FString&, App, const FString&, StreamName, const FTcpEndpoint&, Remote);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDelegateRtmpServerError, const FErrorCode&, ErrorCode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDelegateRtmpServerPlay, const FString&, App, const FTcpEndpoint&, Remote);

/**
 * RTMP server: accepts publishers (OBS, FFmpeg, ...) and makes their audio/video available to receivers
 * (InternetProtocolMedia: UIPRtmpReceiver, iprtmp:// media sources) through FRtmpIngestHub.
 *
 * Playback to RTMP clients (VLC, ffplay, OBS media source, another Unreal) is off by default. Enable it with
 * SetPlaybackEnabled and give each stream a view key with SetPlaybackKey: viewers open
 * rtmp://host:port/<app>/<view key>, so the publishing key is never handed out.
 *
 * StreamName in the events is the stream key sent by the publisher: treat it as a secret (it is never
 * logged by the plugin). Stream and view keys are kept in memory only (never saved properties).
 */
UCLASS(Blueprintable, BlueprintType, Category = "IP|RTMP")
class INTERNETPROTOCOL_API URTMPServer : public UObject
{
	GENERATED_BODY()
public:
	virtual void BeginDestroy() override;

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void AddToRoot();

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void RemoveFromRoot();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	bool IsRooted();

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "IP|RTMP")
	int Backlog = 0;

	/** Opens the listening socket (RTMP default port: 1935). */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	bool Open(const FServerBindOptions& BindOpts);

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void Close();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	bool IsOpen();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	FTcpEndpoint LocalEndpoint();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP")
	FErrorCode GetErrorCode();

	/**
	 * Restricts which stream keys may publish. Empty = any key is accepted.
	 * Kept in memory only (not a saved property) so keys never end up in versioned config.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void SetAllowedStreamKeys(const TArray<FString>& StreamKeys);

	/** Enables RTMPS (RTMP over TLS) for the next Open. Requires a certificate and private key. */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void SetTlsOptions(const FSecurityContextOpts& SecOpts);

	/** Disables RTMPS for the next Open. */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP")
	void ClearTlsOptions();

	/**
	 * Allows RTMP clients to watch published streams (with a view key, see SetPlaybackKey). Off by default.
	 * MaxViewersPerStream: 0 = unlimited (each viewer costs the full stream bitrate in upload). Applies immediately.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP|Playback")
	void SetPlaybackEnabled(bool bEnabled, int32 MaxViewersPerStream = 0);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP|Playback")
	bool IsPlaybackEnabled() const { return bPlaybackEnabled; }

	/**
	 * Lets viewers watch App/StreamKey by opening rtmp://host:port/App/ViewKey. Returns false if ViewKey is empty
	 * or equal to StreamKey (that would reveal the publishing key). Several view keys may map to one stream.
	 */
	UFUNCTION(BlueprintCallable, Category = "IP|RTMP|Playback")
	bool SetPlaybackKey(const FString& App, const FString& StreamKey, const FString& ViewKey);

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP|Playback")
	void RemovePlaybackKey(const FString& App, const FString& ViewKey);

	UFUNCTION(BlueprintCallable, Category = "IP|RTMP|Playback")
	void ClearPlaybackKeys();

	/** Clients currently watching streams of App (empty App = all). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "IP|RTMP|Playback")
	int32 GetViewerCount(const FString& App) const;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServer OnListening;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServerPublish OnPublishStarted;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServerPublish OnPublishEnded;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServer OnClose;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServerError OnError;

	/** A client started watching a stream of App (keys are not reported). */
	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServerPlay OnPlayStarted;

	UPROPERTY(BlueprintAssignable, Category = "IP|RTMP|Events")
	FDelegateRtmpServerPlay OnPlayEnded;

private:
	std::shared_ptr<IPNet::Rtmp::FRtmpServerCore> Core;
	uint64 SessionId = 0;
	uint64 PendingCloseId = 0;
	bool bOpen = false;
	FTcpEndpoint Local;
	FErrorCode LastError;
	TArray<FString> AllowedKeys;
	bool bPlaybackEnabled = false;
	int32 MaxViewers = 0;
	TMap<FString, FString> PlaybackKeys;
	std::shared_ptr<const IPNet::FTlsConfig> Tls;
};
