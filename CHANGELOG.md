# Changelog

## 1.0.35 – 2026-10-31

- **Bitrate per platform.** Recommended follows that platform's cap: Twitch without Partner stays at 6000 kb/s, Kick at 8000 kb/s, and the rest use their own recommended maximum. Typing a video bitrate switches just that destination to Manual.

## 1.0.34 – 2026-10-31

- **The engine tells its version.** The standalone app can show the plugin version next to its own, and download a newer `livedelay.dll` when `version.json` points at one.
- **Pages opened inside the app drop the site menu.** `inapp=1` hides the ribbon and the navigation and tightens the preview and the theme editor to the window.

## 1.0.33 – 2026-10-30

- **The plugin ships the translations it uses.** Thirteen strings in the Audio tab and the per-platform track pickers were missing from the locale files and only existed on the website, so every OBS start downloaded them again — and without a connection those fields showed bare key names. They are in the plugin now, and the locale check no longer has a gap to heal.

## 1.0.32 – 2026-10-30

- **The dock looks like the rest of LiveDelay.** It draws from the product palette, where the three accents are named after what they mean, and fills exactly one button: the action the current state is waiting for. The rest stay outlined. Settings gains a Dock look — same as OBS, LiveDelay dark, or high contrast. The viewer overlay is not repeated there; it keeps its own editor.
- **Web panel from the dock.** A new button opens the full settings page in the browser, the same one the standalone app uses, even though the engine is embedded in OBS.
- **Settings changed elsewhere arrive without a restart.** The dock watches `livedelay.ini`, so a save from the web panel or the app shows up as it happens. A changed platform list or address restarts the engine, but only while nothing is streaming.
- **A platform without autostart really waits.** Start Streaming in OBS no longer starts platforms whose "Start automatically with the OBS stream" is off; they wait for Start in the panel. This reverses the blunt fix from 1.0.7, which started everything so the dock would not sit on "no signal" — that symptom no longer appears, because the dock shows the stream arriving from OBS either way. ([#2](https://github.com/skullboypl/livedelay-obs/issues/2))
- **Kick asks for your own address.** The shipped "Default" was one channel's ingest host, so anyone else connected, briefly looked live, and then failed in a loop — including straight from OBS. Kick gives every channel its own host, so the field is empty and points at your dashboard. An SRT endpoint cannot pass through an RTMP proxy and now says so instead of failing silently. ([#2](https://github.com/skullboypl/livedelay-obs/issues/2))
- **The limits warning stays hidden.** "Hide" is remembered in `livedelay.ini`, keyed to the settings it warned about, so it survives an OBS restart but returns if a genuinely different problem appears. ([#2](https://github.com/skullboypl/livedelay-obs/issues/2))
- **The app's global hotkeys survive a save from OBS.** The settings window rewrote the ini from its own model and dropped `hotkey_*`; those are now carried through untouched.

## 1.0.31 – 2026-10-25

- **Twitch Enhanced Broadcasting (beta).** A Twitch destination can ask Twitch for a GoLive session and send that quality ladder through the delay. Arm, Activate and Cut move every quality together. If Twitch does not return a session, that destination stays on one video track and the normal ingest. Kick and YouTube stay on one track. OBS's own Enhanced Broadcasting checkbox stays off.
- **Dark theme contrast.** Hints, dock status and help text follow the OBS theme color. They no longer render black on the dark theme.

## 1.0.30 – 2026-10-25

- **No presence ping.** The plugin no longer phones home, and the site no longer shows how many copies are open.
- **Audio track per platform.** Each destination picks OBS track 1–6 for what viewers hear. “Same as OBS” keeps Settings → Output → Audio Track.
- **Twitch VOD track.** A different track is sent as the second AAC, so Twitch can keep it in the VOD and out of the way of the live mix. The sequence header is sent again after Arm / Activate / Cut.
- **Tracks per source.** Settings → Audio assigns each OBS audio source to tracks 1–6.
- **What's new, once.** After an update, OBS shows that release's notes a single time. A fresh install stays quiet.

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
