# Changelog

## 0.2.0 – 2026-10-21

- **Auto-update:** the dock checks GitHub Releases (and `livedelay.vxh.pl/version.json`) a few seconds after open, then every 12 hours. A blue banner offers Download / Later; About → Check for updates opens the zip.
- DLL cannot replace itself while OBS is running — Download opens the release zip; close OBS, unzip, restart.

## Unreleased

- **Per-platform encode (option A):** each destination gets its own OBS video encoder and its own delay buffer; stream keys `ld0`, `ld1`, … into the proxy. Stock OBS publish uses `_idle` (discarded).
- Encode modes per platform: Auto, Recommended, Match OBS (performance, default), Manual.
- Dock banner + **Fix/Dopasuj** for Match OBS when Output exceeds platform limits; info tip when 3+ encoders.

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
