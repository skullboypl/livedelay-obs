# LiveDelay for OBS

<sub>**English** · [Polski](README.pl.md)</sub>

Turn a stream delay on and off while you are live, without reconnecting. LiveDelay sits between OBS and your platforms inside OBS itself: hit **Arm**, then **Activate**, and viewers are N seconds behind. Hit **Cut** and they are back to live.

- **One plugin, no extra program.** The delay proxy runs inside OBS (Rust core, about 0.1% CPU and 30 MB RAM for a 15 s delay at 6 Mb/s).
- **Native dock.** A Qt panel, not a browser dock, so it costs next to nothing.
- **Hotkeys.** Arm, Activate, Cut and "Cut after it airs" in Settings → Hotkeys. Bind them to the numpad or a Stream Deck.
- **Several platforms at once.** Twitch, YouTube, Kick, Facebook or any RTMP/RTMPS server, each with its own Start / Stop and an autostart switch.
- **Live preview.** `/live-preview` shows what OBS sends next to what viewers get.
- **Guided setup.** A "How to connect?" walkthrough, and the dock offers to point OBS at LiveDelay when it isn't yet.
- **Overlay.** A Browser source that tells viewers what is going on: LIVE, ARMING…, ARMED, DELAY 25s.
- **English, Polski, Deutsch, Русский.**

![Overlay states](docs/overlay-preview.png)

## How it works

OBS streams to LiveDelay on `rtmp://127.0.0.1:1935/live`. LiveDelay keeps the last few seconds in RAM and sends the stream on to your platforms.

- **Arm** starts keeping a buffer for the delay you set.
- **Activate** jumps viewers back to the keyframe at least N seconds old. Timestamps keep counting up, so players see one continuous stream.
- **Cut** jumps forward to the newest keyframe: back to live.
- **Cut after it airs** marks this moment and cuts once it has reached viewers. Handy after a match ends.

## Install

1. Close OBS.
2. Unzip `livedelay-obs-<version>-windows-x64.zip` into your OBS folder (usually `C:\Program Files\obs-studio`) and confirm merging.
3. Start OBS and open **Tools → LiveDelay**.
4. Click **Route OBS through LiveDelay**. It moves your current server and stream key onto the platform list and points OBS at LiveDelay. Click **Save**.
5. Show the panel with **Docks → LiveDelay**.

Requires OBS Studio 31 or newer on Windows 10/11 x64.

## Overlay

Add a Browser source with `http://127.0.0.1:7799/overlay` (or click **Add to scene** in the settings). Open `http://127.0.0.1:7799/overlay?preview` to see every state at once.

**Look editor:** `http://127.0.0.1:7799/edit-theme` (button in the settings) with five themes (Glass, Minimal, Solid, Neon, Lower third), alignment left / center / right (right-aligned boxes grow to the left), size, font, corners, background, colours per state, capitals, status light and animations, all with a live preview. Saving applies to the overlay on stream immediately.

In the settings each state (Live, Arming, Armed, Delayed) has an on/off switch, its text and a small preview. `{s}` is the delay in seconds, `{buf}` the seconds already buffered. Per-source options in the address: `?size=60`, `?theme=neon`, `?align=right`, `?live=&delayed=ON%20DELAY%20{s}s`. The look is stored in `overlay.json` next to `livedelay.ini`; changing it never restarts the stream.

## Live preview

`http://127.0.0.1:7799/live-preview` (the **Preview** button in the dock) plays the OBS input and the output to viewers side by side, so you can see the delay working. It loads a small player (mpegts.js) from jsDelivr, so it needs internet; H.264 streams play in any browser.

## Stream Deck and scripts

Use the **Hotkey** action with the OBS hotkeys, or call the local API (127.0.0.1 only):

| Request | Does |
|---|---|
| `POST /arm?delay=15` | start buffering for a 15 s delay |
| `POST /activate` | turn the delay on |
| `POST /cut` | back to live now |
| `POST /cut-after` | back to live once this moment has aired |
| `POST /disarm` | drop the buffer |
| `POST /dest/start?i=0`, `POST /dest/stop?i=0` | start or end the stream on one platform (0 = first) |
| `GET /state`, `GET /events` | state as JSON, or pushed as server-sent events |

GET works for the actions too, so the Stream Deck **Website** action can call them. Requests coming from other websites are refused, so a web page cannot cut your stream.

## Known limits

- OBS 31 does not let plugins add their own entry to Settings → Stream → Service, so LiveDelay uses a Custom server (`rtmp://127.0.0.1:1935/live`) and sets it for you.
- No Twitch Enhanced Broadcasting (multitrack). **Route OBS through LiveDelay** switches OBS to a custom server, which turns it off.
- Closing OBS while delayed ends the stream right away: the last N seconds never air.
- Saving the settings while live restarts LiveDelay and drops the stream for a few seconds. The dialog asks first.

## Build

Needs Visual Studio 2022 (C++), Rust (stable, MSVC) and, in `../deps`: obs-studio 31.0.3 sources, Qt 6.6.3 from obs-deps 2024-09-12, and import libraries made from your installed OBS.

```
obs-plugin\make-import-libs.ps1
obs-plugin\build.bat
cargo test
package.ps1
```

`install.ps1` installs a build for the current machine without admin rights (`%ProgramData%\obs-studio\plugins`). A standalone tray app with the same core is in `src/main.rs` (`cargo build --release`).

## Support

LiveDelay is free. If it saves your stream one day: [support the author](https://donate.skullmedia.pl). From the same author: [SmoothWizard](https://smoothwizard.com), an FPS booster for Windows.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
