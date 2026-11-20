# InternetProtocol 4.0 | UE 5.x (tested on 5.8) |

> Version 4.0 requires **Unreal Engine 5** (tested on 5.8); use the 3.x releases for UE 4.27. The live audio/video
> modules (`InternetProtocolMedia`, `InternetProtocolExample`) require **UE 5.8, Win64**.

The InternetProtocol plugin is a versatile networking solution designed for Unreal Engine, enabling developers to seamlessly establish and manage HTTP, WebSocket, TCP, UDP and RTMP connections, including live audio/video receive and publish (see [RTMP and live audio/video](#rtmp-and-live-audiovideo-ue-58-win64)). This powerful plugin caters to the creation of both clients and servers, providing a robust framework for real-time data exchange and online interaction. Whether you’re developing multiplayer games, interactive applications, or complex networked systems, the InternetProtocol plugin offers the essential tools to bring your project’s connectivity to the next level.

This is a development branch to implement some experimental features before merge into master branch.


## Sponsor me

<p align="center">
  <a href="https://www.paypal.com/donate?hosted_button_id=L48BPZ4VVCN6Q"><img src="https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif"></a>
</p>
<p align="center">
  <a href="https://nubank.com.br/pagar/1bcou4/5D6eezlHdm"><img src="https://logodownload.org/wp-content/uploads/2020/02/pix-bc-logo.png" width="128"></a>
</p>

### Suported Platforms

---

- [x] Windows

- [X] Linux(Help Wanted)

- [ ] Mac(Help Wanted)

### Roadmap

---

- [X] TCP server

- [X] TCP client

- [X] UDP server

- [X] UDP client

- [X] Http server

- [X] Http client

- [X] Websocket server

- [X] Websocket client

- [X] SSL support

- [X] RTMP/RTMPS server (ingest + playback with view keys) and clients (publish, play)

- [X] Live audio/video: receive (Electra H.264/AAC, Media Framework) and publish (AVCodecs NVENC/AMF + AAC) - UE 5.8, Win64

### Bug Reporting Template:
```
**Detailed description of issue**
Write a detailed explanation of the issue here.

**Steps To Reproduce:**
1: Detailed Steps to reproduce the issue 
2: Clear steps
3: Etc

**Expected Results:**
A description of what should happen.

**Actual Results:**
A description of what actually happens.
```

# Setting Up Your Project
- Clone the repository or download the latest release.
- Create a C++ empty class if your project is blueprint based.
- Extract/Move `InternetProtocol` folder into your project's `Plugin` folder.

# Migrating from 3.x to 4.0

Run `Scripts/migrate_4_0.py` once (Python Editor Script Plugin, see the header of the script): it recompiles and
re-saves your Blueprints and maps and lists those that need manual fixes. Back up or commit first.

**Classes**
- **TLS on the base classes:** every client and server class has `bUseTls` and `SetTlsOptions(...)`. `UTCPClientSsl`,
  `UTCPServerSsl`, `UWSClientSsl`, `UWSServerSsl`, `UHttpClientSsl` and `UHttpServerSsl` still exist: they are the same
  classes with TLS on.
- **Remotes are shared:** `UTCPRemoteSsl`, `UWSRemoteSsl` and `UHttpRemoteSsl` were merged into `UTCPRemote`,
  `UWSRemote` and `UHttpRemote`. CoreRedirects keep old assets loading; event pins change to the shared class.
- **Deprecated:** `AddToRoot` / `RemoveFromRoot` / `IsRooted` nodes. Keep the object in a variable instead.
- **`Server->Clients()`** returns a copy.

**Behaviour changes (new defaults)**
- **Timeouts:** `ConnectTimeoutSeconds` 15 s on clients (DNS + connect + TLS + WebSocket upgrade);
  `HandshakeTimeoutSeconds` 10 s on servers; HTTP servers close idle connections after 60 s; an unanswered HTTP
  request gets 503 after `ResponseTimeoutSeconds` (30 s).
- **WebSocket keep-alive:** clients and remotes ping after 20 s of silence and close after 60 s without data
  (`PingIntervalSeconds` / `IdleTimeoutSeconds`, 0 disables).
- **Limits:** servers accept up to `MaxConnections` (1024); a connection stops reading when the Game Thread is more
  than `MaxPendingReceiveMB` (8) behind (UDP drops instead).
- **Sockets:** `EProtocolType::Any` is the default (IPv4 and IPv6); TCP sockets use `bNoDelay`; the UDP receive
  buffer is 64 KiB.
- **HTTP:** the parser rejects malformed headers (bare LF, folded lines, spaces before ":"), HTTP/1.1 requests
  without Host, and ambiguous framing. Routes are case-sensitive.
- **WebSocket:** close codes that cannot be sent are replaced by 1000.

**New features (highlights)**
- **Errors and stats:** `FErrorCode.Category`; `GetStats()` on connections.
- **TCP framing:** `Framing` with `LengthPrefixed` or `Delimiter` modes.
- **UDP:** broadcast and multicast.
- **WebSocket:**
  - `ConnectUrl`, fragmented messages (`bFin = false`), subprotocols;
  - `OnUpgradeRequest` with `AcceptUpgrade` / `RejectUpgrade`;
  - automatic reconnection, ping round trip.
- **HTTP client:**
  - binary bodies (`BodyBytes`) and repeated headers (`HeaderList`, e.g. several Set-Cookie);
  - `Request` returns an id for `Cancel`, per-request `TimeoutSeconds`;
  - `OnProgress`, redirects (`MaxRedirects`), `SetUrl`.
- **HTTP server:**
  - route parameters (`/users/:id`, `/files/*` in `RouteParams`), 405, HEAD via GET, 100-continue;
  - streamed responses (`BeginStream` / `WriteChunk` / `EndStream`).
- **JSON:** exact int64 values; missing fields no longer crash.

# RTMP and live audio/video (UE 5.8, Win64)

RTMP support is split in two layers:

| Module | What it does | Depends on |
|---|---|---|
| `InternetProtocol` | RTMP/RTMPS protocol: `URTMPServer` (ingest + playback), publishing client (`IRtmpPublishSession`), playback client (`IRtmpPlaySession`), `FRtmpIngestHub`. No media dependencies. | Asio, OpenSSL |
| `InternetProtocolMedia` | Decoding, playback in the Media Framework, capture and encoding: `UIPRtmpReceiver`, `UIPRtmpPublisher`, `UIPRtmpMediaSource`. | Electra (decoders), AVCodecs (encoders), Media Framework |
| `InternetProtocolExample` | Example actor and UI used by the demo map. Not needed by games. | UMG |

Only H.264 video and AAC audio are supported, which covers OBS, FFmpeg, MediaMTX, nginx-rtmp, Twitch and YouTube.

## Receive a stream (OBS / FFmpeg -> Unreal)

```cpp
URTMPServer* Server = NewObject<URTMPServer>(this);
FServerBindOptions Bind;
Bind.Address = TEXT("0.0.0.0");
Bind.Port = 1935;
Server->Open(Bind);

UIPRtmpReceiver* Receiver = NewObject<UIPRtmpReceiver>(this);
Receiver->StartFromServer(Server, TEXT("live"), StreamKey, EIPRtmpLatencyMode::LowLatency);
ScreenMaterial->SetTextureParameterValue(TEXT("Video"), Receiver->GetVideoTexture()); // UMediaTexture
Receiver->CreateAudioComponent(ScreenActor);                                          // spatialized sound
```

In OBS, set **Settings > Stream** to *Custom*, Server `rtmp://<unreal machine>:1935/live`, and use your stream key. Use x264 or NVENC H.264 with a keyframe interval of 1 to 2 s, and AAC audio.

- **Latency:** `LowLatency` targets about 150 ms of jitter buffer and `Smooth` about 1000 ms (configurable). Audio is the master clock.
- **Media Framework:** `UIPRtmpMediaSource` / `iprtmp://0.0.0.0:1935/live/<key>` also works with `UMediaPlayer`, `UMediaTexture`, `UMediaSoundComponent` and Media Plate.
- **Events and stats:** `OnStarted`, `OnEnded`, `OnError` and `OnFrame` (metadata only). `GetStats()` reports fps, bitrate, buffer, drops and decoders.

## Let RTMP players watch (Unreal server -> VLC / ffplay / OBS)

Playback is **off by default**. Viewers use a **view key**, so the publishing key is never handed out:

```cpp
Server->SetPlaybackEnabled(true, /*MaxViewersPerStream*/ 0);          // 0 = unlimited
Server->SetPlaybackKey(TEXT("live"), StreamKey, TEXT("watch-123"));    // view key must differ from the stream key
```

To watch, open `rtmp://<unreal machine>:1935/live/watch-123` in VLC, `ffplay`, or an OBS *Media Source*. Each viewer costs the full stream bitrate in upload bandwidth.

Each viewer has a bounded send queue: under congestion P-frames are dropped and the stream resumes at the next keyframe. Viewers stay connected while the publisher reconnects. Use `OnPlayStarted`, `OnPlayEnded` and `GetViewerCount` to follow them.

## Receive from another RTMP server (pull)

```cpp
Receiver->StartPull(TEXT("rtmp://media.example.com:1935/live"), StreamKey, EIPRtmpLatencyMode::Smooth);
```

`rtmp://` and `rtmps://` are supported. The stream key is kept out of the URL and never logged. Reconnection uses exponential backoff. A wrong key (`StreamNotFound` / `Unauthorized`) is reported through `OnError` and is not retried.

For `rtmps://`, certificates are always verified: set CA certificates through the native `FRtmpPlayOptions::Tls` if needed.

## Publish from Unreal (Unreal -> MediaMTX / nginx-rtmp / Twitch / YouTube)

```cpp
FIPRtmpPublishConfig Config;
Config.VideoSource = EIPPublishVideoSource::Viewport;   // or RenderTarget / SceneCapture
Config.Width = 1280; Config.Height = 720; Config.FrameRate = 30;
Config.VideoBitrateKbps = 4500; Config.KeyframeIntervalSeconds = 2;
Config.AudioSource = EIPPublishAudioSource::Submix;     // main submix, or Manual (PushAudio) / None

UIPRtmpPublisher* Publisher = NewObject<UIPRtmpPublisher>(this);
Publisher->Start(TEXT("rtmp://live.twitch.tv/app"), StreamKey, Config);
```

- **Video:** H.264 encoded by the GPU through the engine's AVCodecs: NVENC first, then AMF. There is no CPU readback. Frames are captured at the configured rate, up to the game frame rate.
- **Audio:** AAC-LC at 48 kHz stereo, encoded by Media Foundation. Bitrate is 96, 128, 160 or 192 kbps.
- **Network:**
  - `rtmp://` and `rtmps://`, with a bounded send queue.
  - Under congestion P-frames are dropped first.
  - Reconnection uses exponential backoff from 1 to 30 s, and a keyframe is forced after each reconnection.
- **Events and stats:** `OnStateChanged`, `OnPublishing`, `OnReconnecting` and `OnError`. `GetStats()` reports fps, sent bitrate, drops, queue, RTT and reconnections.

## Security

- Stream keys and view keys are never logged, never `UPROPERTY` and never saved: URLs are logged as `rtmp://host/app/***`.
- Avoid putting keys in URLs stored in assets; use `StartPull(Url, Key)`, `UIPRtmpMediaSource::SetStreamKey` or `Start(Url, Key, Config)`.
- `URTMPServer::SetAllowedStreamKeys` restricts which keys may publish.
- RTMPS uses the plugin's TLS layer. Certificate verification is never disabled by the RTMP code.

## Example map

Open `/InternetProtocol/Example/RTMP/RtmpExample` and press Play. Tab switches between the panel and moving around (WASD + mouse). The panel lets you:

- **Receive:** start a server and receiver, with an optional view key for VLC.
- **Play from a server:** pull any RTMP server's stream onto the screen.
- **Publish:** stream the viewport, the camera (SceneCapture) or a render target.

A light flash and a beep every second help judge audio/video sync. The map and its material are generated by `Scripts/create_rtmp_example.py` (Python Editor Script Plugin), which you can re-run to rebuild them.

## Requirements and known limitations

- **Engine version:** UE 5.8, Win64 only. The RTMP media modules use Electra and AVCodecs APIs from 5.8.
- **Encoders:** AVCodecs is **Experimental** in UE 5.8, so its API may change in later versions. The encoders are behind `IIPVideoEncoder` / `IIPAudioEncoder` (`FIPMediaEncoders::Set*EncoderFactory`), so they can be replaced.
- **No software H.264 encoder:** publishing needs an NVIDIA (NVENC) or AMD (AMF) GPU. Otherwise `Start` reports a clear error through `OnError`.
- **Publishing requires the D3D12 RHI.** AVCodecs' D3D11 path removes the GPU device when it fails to open, so on D3D11 `Start` refuses with a clear error instead. Receiving works on both D3D11 and D3D12.
  - A GPU without SM6 (e.g. GTX 750 Ti) falls back to D3D11 when the project targets only `PCD3D_SM6` for D3D12.
  - To keep D3D12 on such GPUs, also target SM5: Project Settings > Platforms > Windows > Targeted RHIs > *DirectX 12 (SM5)*, i.e. `+D3D12TargetedShaderFormats=PCD3D_SM5`.
- **Decode speed follows the engine frame rate:** Electra hands decoded pictures to the GPU through the RHI. If the game runs slower than the stream (or is throttled by Windows while in the background), frames are dropped.
  - Windows' background power throttling (EcoQoS) can cut a background Unreal process to about 20 fps.
  - For reliable results keep the window in the foreground, or use the *Best performance* power mode.
- **Decoders:** they can be replaced through `IIPVideoDecoder` / `IIPAudioDecoder` (`FIPMediaDecoders`).
- **Not supported:** `play2`, HEVC/AV1 over RTMP (Enhanced RTMP), and recording to disk.

## Tests

Automation tests live under `InternetProtocol.*` (Session Frontend, or `-ExecCmds="Automation RunTests InternetProtocol"`):

- **RTMP protocol:** AMF0, chunks, FLV, handshake, publish/play loopback over TCP and TLS, view keys and refusals.
- **Media:** playout clock, audio ring, AVCC, H.264/AAC encoders, FFmpeg -> Unreal (push and pull), publisher -> FFmpeg, publisher -> receiver loopback, and the example map in PIE.

Tests that need FFmpeg (in `PATH`), a real RHI (no `-nullrhi`) or an NVIDIA/AMD GPU are skipped with a warning otherwise. The receive tests check frame rates, so run them with the editor unthrottled:
`-ini:EditorPerProjectUserSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`.

# Thirdparty libraries
- [Asio non-boost](https://think-async.com/Asio/)
- [Openssl](https://www.openssl.org/)

# LICENCE

InternetProtocol is licensed under the GNU Affero General Public License, version 3 (AGPL v3).  
This means you are free to use, modify, and distribute this software, provided that you comply with the terms of the
AGPL v3, which ensures that any modified versions or derivative works are also open-source and licensed under the AGPL
v3.

See the full license text in the `LICENSE` file or
at [https://www.gnu.org/licenses/agpl-3.0.txt](https://www.gnu.org/licenses/agpl-3.0.txt).

## Support the Project

If you find this project useful or want to use the code without (AGPL v3) limitation, purchasing a copy
on [Fab.com](https://www.fab.com) provided by Epic Games. Your support helps fund continued development and maintenance.
