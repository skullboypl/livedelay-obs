# Changelog

## 1.0.9 – 2026-10-21

- **One-command install:** `irm https://livedelay.vxh.pl/install.ps1 | iex` downloads the latest zip and installs into ProgramData (detects OBS / Steam / portable).
- **Install-LiveDelay.ps1** in the release zip: pick OBS folder, ProgramData layout (OBS 31–33+), portable `-IntoObsFolder`.
- Docs/README lead with the one-liner; zip-into-Program-Files is no longer the default path (common cause of “plugins failed to load”).

## 1.0.8 – 2026-10-21

- **Twitch #1000:** encoder-specific presets (no NVENC `hq` on x264) + classic Twitch RTMP capped at 1080p.
- Idle OBS publish counts as connected in the dock; Start Streaming starts all enabled destinations with URL+key.

## 1.0.7 – 2026-10-21

- Start Streaming always starts enabled platforms (ignore `autostart=no` alone leaving only `_idle` / “no signal”).

## 1.0.6 – 2026-10-21

- **Bulletproof platform start:** legacy OBS key `livedelay` no longer steals slot `ld0` (was `publisher busy` + Twitch player #1000). Stock publish is always discarded (`_idle` / empty / `livedelay`).
- Harden on every Start Streaming: force stock key `_idle`, Multitrack off; delay + retry when starting per-platform encoders so the local RTMP race cannot kill Twitch.

## 1.0.5 – 2026-10-21

- **Twitch #1000 hardening:** force H.264 (never HEVC/AV1) on classic Twitch RTMP; reseed codec configs after egress lag; wait for video before dialing Twitch.

## 1.0.4 – 2026-10-21

- **One-click auto-update:** Update downloads the zip, closes OBS, replaces the plugin (ProgramData or Program Files layout), and restarts OBS. No manual unzip.

## 1.0.3 – 2026-10-21

- **Twitch player #1000 guard:** never publish multi-video Enhanced Broadcasting over stock RTMP (breaks Twitch without GoLive). Always one video track; EB checkbox disabled until GoLive.
- Re-harden Multitrack-off on every OBS Start Streaming.

## 1.0.2 – 2026-10-21

- **No more Twitch drop on Save:** changing overlay look, delay, buffer or language no longer restarts the proxy or kills RTMP outputs.
- **Route OBS fix:** Custom RTMP URL + key set correctly; stock Enhanced Broadcasting (Multitrack) disabled so Start Streaming no longer shows “missing config URL”.
- **VOD track:** EnableCustomServerVodTrack + AdvOut Vod Track when a Twitch dest asks for VOD audio.
- Hot-apply `POST /settings?delay=&max_buffer_mb=` without tearing down destinations.

## 1.0.0 – 2026-10-21

- **Buffer RAM helper:** Settings show estimated MB for delay × bitrate, status colours, and **Use recommended**.
- **Safer Arm:** refuses when the RAM limit is too small for the chosen delay; soft eviction so the buffer does not balloon.
- **Auto-update (fixed):** dock banner with **Update**; checks `livedelay.vxh.pl/version.json` then GitHub via WinHTTP (no Qt TLS failure inside OBS).
- Clearer About tab and Polish/EN/DE/RU copy; delay buffer documented as RAM (longer delay = more memory).

## 0.2.0 – 2026-10-21

- **Auto-update:** the dock checks GitHub Releases (and `livedelay.vxh.pl/version.json`) a few seconds after open, then every 12 hours. A blue banner offers Download / Later; About → Check for updates opens the zip.
- DLL cannot replace itself while OBS is running — Download opens the release zip; close OBS, unzip, restart.

## 0.1.0 – 2026-10-21

First release.

- Delay proxy inside OBS: Arm, Activate, Cut, Cut after it airs, on keyframes, with continuous timestamps.
- Native dock and four OBS hotkeys.
- Settings window: default delay, buffer memory limit, platforms with presets (Twitch, YouTube, Kick, Facebook), "Route OBS through LiveDelay".
- Several platforms at once over RTMP and RTMPS.
- Browser-source overlay: five themes, alignment, look editor at `/edit-theme`, per-state on/off, `?preview`.
- Local HTTP API for Stream Deck and scripts.
- Per-platform Start / Stop and autostart.
- `/live-preview`: OBS input and viewer output side by side.
- "How to connect?" walkthrough; the dock offers to route OBS when it isn't.
- "Waiting for OBS stream" state.
- English, Polish, German and Russian.
