# Changelog

## 1.0.43 – 2026-11-11

- **Inputs and outputs in the plugin.** Settings has an **Inputs** tab and the platforms tab is now **Outputs (platforms)**. Inputs lists OBS itself and any input another program feeds; each output picks its input: *OBS — own encode* (as before), *OBS — shared encode*, or one of the others.
- **Shared encode.** Platforms ticked under Inputs → OBS get one stream: OBS encodes and uploads the picture once instead of once per platform. The plugin starts one encoder for them, keeps it running while any of them is live, and the note about separate encodes counts encodes, not platforms. Nothing changes for a settings file that does not use it.
- **Vertical stream wizard.** Tools → *LiveDelay: vertical stream*. It tells you whether Aitum Vertical is loaded in this OBS, makes an input for it, gives you the address and key to paste, waits until the picture actually arrives — "I can see 720×1280, 2500 kb/s", and whether it is really vertical — and then points platforms at it. The plugin never starts an encoder of its own on an input another program feeds.
- **Delay shared or per input.** Inputs → Delay. Per input, each input gets one button in the dock that does the next thing for it: arm, switch the delay on, go back live. The big buttons still act on everything.
- **The dock groups platforms under a shared input** and stays as it was — one line for OBS, then the platforms — while every platform encodes for itself.
- **Single PC feeds every platform on the shared encode,** not only the first one. Saving with Single PC on and several separate encodes asks whether to switch them to the shared one.
- **The plugin no longer mangles inputs written by the app or the web panel.** Its settings reader knew only platforms, so `key = ld1` under `[input …]` was read as the stream key of the platform above it, and a save dropped the section. It now reads inputs, and keeps every key and section it does not know.

## 1.0.42 – 2026-11-11

- **The update from 1.0.41 sticks.** 1.0.41 carried the right code but still called itself 1.0.40, so the dock kept offering the same update after it had been installed. The version now comes from one place in the build, so the two cannot drift apart again.
- **Each platform is listed once in the dock.** With the 1.0.41 engine the dock also read every input as a platform — an older settings file gets one input per platform, named after it, so Twitch and Kick appeared twice. Worse, the Live and Stop buttons on the real rows were shifted and could act on the wrong platform. The dock now reads platforms from the platform list only.

## 1.0.41 – 2026-11-11

- **One input can feed many platforms.** An input is now a thing of its own — `[input Nazwa]` with its own publish key — and a platform says which one feeds it with `input = Nazwa`. Twitch and YouTube can share one horizontal picture while TikTok takes a vertical one, so OBS encodes and uploads it once instead of once per platform. A file without `[input …]` is unchanged: one input per platform, on its own key, exactly as before. `/state` carries an `inputs` array with each input's rate, picture size and buffer; `/arm`, `/activate`, `/cut`, `/cut-after` and `/disarm` take an optional `?input=` and without it still act on everything, so Stream Deck keeps working. `delay_mode = per_input` tells the interfaces to show one set of buttons per input.
- **Chat, activity and stream info as OBS docks.** Each platform offers its panels as tick boxes instead of addresses to paste: chat, the activity list and the page that changes the title and category, with the stream manager, moderation and analytics for whoever wants them. Every address was checked against a live account. Signing in happens in OBS's own cookie jar — LiveDelay neither sees nor stores it.
- **The settings file keeps what this build does not understand.** A section from a newer or older build used to lose its header while its keys were read as global settings, so `delay = 20` under such a section quietly became the global delay and the file was rewritten into something that meant something else. Unknown sections are now copied back word for word and their keys applied to nothing.
- **The web panel can create inputs, not only read them.** `POST /config` writes `[input …]` sections and points a platform at one, refusing a name that cannot be written back, two inputs sharing a publish key, or a platform naming an input that is not there.

## 1.0.40 – 2026-11-04

- **New settings show their names again.** An update of the flat plugin DLL wrote the locale files next to OBS instead of next to the plugin, so OBS kept the old file and the new rows appeared as `Encode.BufferCodec`. The install now puts those files in the plugin folder, and a missing line is downloaded on startup.

## 1.0.39 – 2026-11-04

- **Start Streaming no longer swaps the OBS service mid-start.** A stock key that was not `_idle` used to rebuild the service while OBS was already starting, so `rtmp_output` failed with no error and platforms never connected.
- **Twitch is not Kick.** The automatic Twitch ingest on `ingest.global-contribute.live-video.net` no longer forces H.264. NVENC stays on the codec OBS is already using unless Kick is enabled and the stream is actually going there.
- **Buffer codec.** Settings and the web panel can keep OBS's encoder or pick H.264, HEVC or AV1. The choice stays on the same GPU encoder. It does not fall back to x264.
- **Single PC.** One checkbox makes OBS's own encode the delay buffer, so a game on the same machine does not pay for a second NVENC session.

## 1.0.38 – 2026-10-31

- **The VOD track stays on Twitch.** Kick, YouTube, Facebook and a custom RTMP server get a checkbox instead. LiveDelay asks before showing the picker, because it does not expect that platform to keep a separate recording track. Confirming sends the second AAC anyway.
- **The separate-encode note only appears when the settings differ.** Three platforms that resolve to the same encoder, bitrate, keyframe interval and size no longer show the dock card about encoding each one apart.

## 1.0.37 – 2026-10-31

- **Live and VOD tracks stay on every platform.** The VOD picker was switched off and cleared whenever the destination was not Twitch, so it also vanished when Twitch was not recognized for a moment. Both fields stay. The tooltip says the second recording track works on Twitch and other platforms ignore it. Only Twitch still receives that second AAC.

## 1.0.36 – 2026-10-31

- **Three platforms no longer flood the log.** The dock note about a separate encoder was passed through `QString::arg` with no placeholder, so OBS logged a warning on every refresh. ([#3](https://github.com/skullboypl/livedelay-obs/issues/3))
- **Kick is not Twitch.** A Kick ingest on `live-video.net` no longer shows the Twitch VOD track or Enhanced Broadcasting, and the address stays empty because each channel has its own. A repeated local key takes the next free `ldN`, so two platforms can no longer publish as `ld2`. ([#3](https://github.com/skullboypl/livedelay-obs/issues/3))

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
