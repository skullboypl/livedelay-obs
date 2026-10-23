//! `livedelay.ini`: a few `key = value` lines and one `[dest NAME]` section per destination.

use std::path::Path;

pub const EXAMPLE: &str = "\
# LiveDelay settings (in OBS: Tools -> LiveDelay)

listen = 127.0.0.1:1935
api = 127.0.0.1:7799
# delay in seconds used by Arm
delay = 15
# memory limit for the buffer (MB). Split across platforms when multi-encoding.
max_buffer_mb = 512

# Overlay texts (Browser source: http://127.0.0.1:7799/overlay).
# {s} = delay in seconds, {buf} = how much is buffered. Empty = show nothing.
overlay_live = LIVE
overlay_arming = ARMING… {buf}/{s}s
overlay_armed = ARMED {s}s
overlay_delayed = DELAY {s}s

# Global hotkeys for the standalone tray app, e.g. Ctrl+Alt+Num1, F13, Shift+F5.
hotkey_arm =
hotkey_activate =
hotkey_cut =
hotkey_cut_after =

# One block per platform. enabled = no turns it off without deleting it,
# autostart = no waits for Start in LiveDelay instead of following OBS.
# encode_mode: auto | recommended | obs | manual  (obs = match OBS Output)
[dest Twitch]
url = rtmp://live.twitch.tv/app
key = PASTE_YOUR_KEY
ingest_key = ld0
encode_mode = obs
enabled = no
autostart = yes
";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum EncodeMode {
    Auto,
    Recommended,
    TwitchPartner,
    /// Mirror OBS Settings → Output → Streaming (performance / default).
    Obs,
    Manual,
}

impl EncodeMode {
    pub fn parse(s: &str) -> Option<Self> {
        match s.to_ascii_lowercase().as_str() {
            "auto" => Some(Self::Auto),
            "recommended" | "zalecane" => Some(Self::Recommended),
            "twitch_partner" | "partner" | "zalecane_partner" => Some(Self::TwitchPartner),
            "obs" | "output" | "performance" => Some(Self::Obs),
            "manual" | "recznie" | "ręcznie" => Some(Self::Manual),
            _ => None,
        }
    }

    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Recommended => "recommended",
            Self::TwitchPartner => "twitch_partner",
            Self::Obs => "obs",
            Self::Manual => "manual",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Dest {
    pub name: String,
    pub url: String,
    /// Which row the settings window showed: `auto`, a server name, or empty.
    /// The proxy dials `url`; this only lets the window tell Auto from the
    /// ingest that happens to share Auto's address.
    pub server: String,
    pub key: String,
    pub enabled: bool,
    /// Start sending as soon as OBS streams; otherwise wait for Start in LiveDelay.
    pub autostart: bool,
    /// RTMP publish key into this proxy (`ld0`, …). Empty → assigned as `ld{index}` at start.
    pub ingest_key: String,
    pub encode_mode: EncodeMode,
    pub video_kbps: u32,
    pub audio_kbps: u32,
    pub keyint_sec: u32,
    pub rate_control: String,
    pub width: u32,
    pub height: u32,
    pub vod_audio: bool,
    pub enhanced_broadcasting: bool,
}

impl Default for Dest {
    fn default() -> Self {
        Self {
            name: String::new(),
            url: String::new(),
            server: String::new(),
            key: String::new(),
            enabled: true,
            autostart: true,
            ingest_key: String::new(),
            encode_mode: EncodeMode::Obs,
            video_kbps: 0,
            audio_kbps: 0,
            keyint_sec: 0,
            rate_control: "CBR".into(),
            width: 0,
            height: 0,
            vod_audio: false,
            enhanced_broadcasting: false,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Overlay {
    pub live: String,
    pub arming: String,
    pub armed: String,
    pub delayed: String,
}

impl Default for Overlay {
    fn default() -> Self {
        Self {
            live: "LIVE".into(),
            arming: "ARMING… {buf}/{s}s".into(),
            armed: "ARMED {s}s".into(),
            delayed: "DELAY {s}s".into(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Config {
    pub listen: String,
    pub api: String,
    pub delay_s: f64,
    pub max_buffer_mb: usize,
    pub dests: Vec<Dest>,
    /// (action, key combo) pairs for global hotkeys.
    pub hotkeys: Vec<(&'static str, String)>,
    pub overlay: Overlay,
    /// overlay.json next to the ini; empty when the config came from a string.
    pub overlay_file: std::path::PathBuf,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            listen: "127.0.0.1:1935".into(),
            api: "127.0.0.1:7799".into(),
            delay_s: 15.0,
            max_buffer_mb: 512,
            dests: vec![],
            hotkeys: vec![],
            overlay: Overlay::default(),
            overlay_file: Default::default(),
        }
    }
}

fn yes(v: &str) -> bool {
    matches!(v.to_ascii_lowercase().as_str(), "yes" | "tak" | "true" | "1" | "on")
}

pub struct ParseReport {
    pub config: Config,
    /// Lines that could not be applied. The rest of the file is still used.
    pub dropped: Vec<String>,
    /// Original text with the dropped lines removed.
    pub repaired: String,
}

fn backup_path(path: &Path) -> std::path::PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(".bak");
    path.with_file_name(name)
}

fn broken_path(path: &Path) -> std::path::PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(".broken");
    path.with_file_name(name)
}

/// Reads the ini. A bad line is skipped and listed in `dropped`; unknown keys
/// stay in `repaired` so a newer or older build does not wipe them.
pub fn parse_report(text: &str) -> ParseReport {
    let text = text.trim_start_matches('\u{feff}');
    let mut c = Config::default();
    let mut dropped = Vec::new();
    let mut kept: Vec<&str> = Vec::new();
    let mut in_dest = false;
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        let note = |m: &str| format!("livedelay.ini, line {}: {m}", n + 1);
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            kept.push(raw);
            continue;
        }
        if line.starts_with('[') {
            if let Some(sec) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
                if let Some(name) = sec.trim().strip_prefix("dest") {
                    in_dest = true;
                    c.dests.push(Dest { name: name.trim().to_string(), ..Dest::default() });
                    kept.push(raw);
                } else {
                    in_dest = false;
                    dropped.push(note("a section must look like [dest Name]"));
                }
            } else {
                in_dest = false;
                dropped.push(note("a section must look like [dest Name]"));
            }
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            dropped.push(note("brak znaku ="));
            continue;
        };
        let (k, v) = (k.trim(), v.trim());
        let mut keep = true;
        if in_dest {
            if let Some(d) = c.dests.last_mut() {
                match k {
                    "url" => d.url = v.into(),
                    "server" => d.server = v.into(),
                    "key" => d.key = v.into(),
                    "enabled" => d.enabled = yes(v),
                    "autostart" => d.autostart = yes(v),
                    "ingest_key" => d.ingest_key = v.into(),
                    "encode_mode" => match EncodeMode::parse(v) {
                        Some(m) => d.encode_mode = m,
                        None => {
                            keep = false;
                            dropped.push(note(
                                "encode_mode must be auto, recommended, twitch_partner, obs or manual",
                            ));
                        }
                    },
                    "video_kbps" => match v.parse() {
                        Ok(n) => d.video_kbps = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("video_kbps must be a number"));
                        }
                    },
                    "audio_kbps" => match v.parse() {
                        Ok(n) => d.audio_kbps = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("audio_kbps must be a number"));
                        }
                    },
                    "keyint_sec" => match v.parse() {
                        Ok(n) => d.keyint_sec = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("keyint_sec must be a number"));
                        }
                    },
                    "rate_control" => d.rate_control = v.into(),
                    "width" => match v.parse() {
                        Ok(n) => d.width = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("width must be a number"));
                        }
                    },
                    "height" => match v.parse() {
                        Ok(n) => d.height = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("height must be a number"));
                        }
                    },
                    "vod_audio" => d.vod_audio = yes(v),
                    "enhanced_broadcasting" | "enhanced" => d.enhanced_broadcasting = yes(v),
                    // Which OBS service the URL came from; only the settings window uses it.
                    "service" => {}
                    _ => {}
                }
            }
        } else {
            match k {
                "listen" => c.listen = v.into(),
                "api" => c.api = v.into(),
                "delay" => match v.parse() {
                    Ok(n) => c.delay_s = n,
                    Err(_) => {
                        keep = false;
                        dropped.push(note("delay must be a number"));
                    }
                },
                "max_buffer_mb" => match v.parse::<usize>() {
                    Ok(n) => c.max_buffer_mb = n,
                    Err(_) => {
                        keep = false;
                        dropped.push(note("max_buffer_mb must be a number"));
                    }
                },
                // Plugin UI language; the proxy itself has no texts to translate.
                "language" => {}
                "overlay_live" => c.overlay.live = v.into(),
                "overlay_arming" => c.overlay.arming = v.into(),
                "overlay_armed" => c.overlay.armed = v.into(),
                "overlay_delayed" => c.overlay.delayed = v.into(),
                // Plugin-only: seconds for the "arm this delay" hotkeys. The proxy does not bind keys.
                k if k.starts_with("arm_slot_") => {}
                "hotkey_arm" | "hotkey_activate" | "hotkey_cut" | "hotkey_cut_after" => {
                    let action = match k {
                        "hotkey_arm" => "arm",
                        "hotkey_activate" => "activate",
                        "hotkey_cut" => "cut",
                        _ => "cut-after",
                    };
                    if !v.is_empty() {
                        c.hotkeys.push((action, v.to_string()));
                    }
                }
                _ => {}
            }
        }
        if keep {
            kept.push(raw);
        }
    }
    for (i, d) in c.dests.iter_mut().enumerate() {
        if d.ingest_key.is_empty() {
            d.ingest_key = format!("ld{i}");
        }
    }
    let mut repaired = kept.join("\n");
    if text.ends_with('\n') || text.ends_with("\r\n") {
        repaired.push('\n');
    }
    ParseReport { config: c, dropped, repaired }
}

pub fn parse(text: &str) -> Result<Config, String> {
    Ok(parse_report(text).config)
}

fn finish(path: &Path, mut c: Config) -> Config {
    c.overlay_file = path.with_file_name("overlay.json");
    c
}

fn read_text(path: &Path) -> Result<String, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    if bytes.contains(&0) {
        return Err(format!("{} is not a text file", path.display()));
    }
    String::from_utf8(bytes).map_err(|_| format!("{} is not UTF-8", path.display()))
}

/// Copies `livedelay.ini.bak` over `path` when the backup itself parses.
fn restore_backup(path: &Path) -> Result<Config, String> {
    let bak = backup_path(path);
    let text = read_text(&bak).map_err(|_| {
        format!(
            "settings file {} is damaged and there is no usable {}",
            path.display(),
            bak.display()
        )
    })?;
    let report = parse_report(&text);
    if report.config.dests.is_empty() && !text.contains("[dest") && report.dropped.len() > 4 {
        return Err(format!("backup {} is damaged too", bak.display()));
    }
    let broken = broken_path(path);
    let _ = std::fs::rename(path, &broken);
    std::fs::copy(&bak, path).map_err(|e| format!("cannot restore {} from backup: {e}", path.display()))?;
    log!(
        "settings file was damaged; restored {} (the broken copy is {})",
        path.display(),
        broken.display()
    );
    Ok(finish(path, report.config))
}

/// Loads the config, writing the commented example on first run.
/// A damaged file is repaired in place (original kept as `livedelay.ini.bak`).
/// A file that is not text at all is replaced from that backup.
pub fn load(path: &Path) -> Result<Config, String> {
    if !path.exists() {
        std::fs::write(path, EXAMPLE).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    }
    let text = match read_text(path) {
        Ok(t) if !t.trim().is_empty() => t,
        _ => {
            return match restore_backup(path) {
                Ok(c) => Ok(c),
                Err(e) => {
                    log!("{e}");
                    let broken = broken_path(path);
                    let _ = std::fs::rename(path, &broken);
                    std::fs::write(path, EXAMPLE)
                        .map_err(|err| format!("cannot write {}: {err}", path.display()))?;
                    log!(
                        "settings file was empty or unreadable; wrote a fresh example (the old file is {})",
                        broken.display()
                    );
                    Ok(finish(path, parse(EXAMPLE)?))
                }
            };
        }
    };
    let report = parse_report(&text);
    if !report.dropped.is_empty() {
        let bak = backup_path(path);
        match std::fs::write(&bak, &text) {
            Ok(()) => {
                if let Err(e) = std::fs::write(path, &report.repaired) {
                    log!("settings repair could not be saved: {e}");
                } else {
                    log!(
                        "settings file had {} problem(s); original saved as {}",
                        report.dropped.len(),
                        bak.display()
                    );
                    for d in &report.dropped {
                        log!("settings repair: {d}");
                    }
                }
            }
            Err(e) => log!("settings problem but the backup could not be written: {e}"),
        }
    }
    Ok(finish(path, report.config))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn example_parses() {
        let c = parse(EXAMPLE).unwrap();
        assert_eq!(c.delay_s, 15.0);
        assert_eq!(c.dests.len(), 1);
        assert!(!c.dests[0].enabled);
        assert_eq!(c.dests[0].encode_mode, EncodeMode::Obs);
        assert_eq!(c.dests[0].ingest_key, "ld0");
    }

    #[test]
    fn bad_line_keeps_the_rest_and_the_server_pick() {
        let text = "\
delay = x

[dest Twitch]
url = rtmp://live.twitch.tv/app
key = abc
server = auto
encode_mode = nope
";
        let report = parse_report(text);
        assert!(report.dropped.iter().any(|d| d.contains("line 1")));
        assert!(report.dropped.iter().any(|d| d.contains("encode_mode")));
        assert_eq!(report.config.delay_s, 15.0);
        assert_eq!(report.config.dests.len(), 1);
        assert_eq!(report.config.dests[0].url, "rtmp://live.twitch.tv/app");
        assert_eq!(report.config.dests[0].key, "abc");
        assert_eq!(report.config.dests[0].server, "auto");
        assert_eq!(report.config.dests[0].encode_mode, EncodeMode::Obs);
        assert!(!report.repaired.contains("delay = x"));
        assert!(report.repaired.contains("server = auto"));
        assert!(report.repaired.contains("key = abc"));
    }

    #[test]
    fn unknown_key_does_not_reject_the_file() {
        let c = parse("future_flag = 1\n[dest Kick]\nurl = rtmps://example/app\nkey = k\n").unwrap();
        assert_eq!(c.dests.len(), 1);
        assert_eq!(c.dests[0].key, "k");
    }

    #[test]
    fn unreadable_file_falls_back_to_backup() {
        let dir = std::env::temp_dir().join(format!("livedelay-ini-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("livedelay.ini");
        let bak = dir.join("livedelay.ini.bak");
        std::fs::write(&bak, "[dest Twitch]\nurl = rtmp://live.twitch.tv/app\nkey = secret\n").unwrap();
        std::fs::write(&ini, b"\0not an ini").unwrap();
        let c = load(&ini).unwrap();
        assert_eq!(c.dests[0].key, "secret");
        assert!(!std::fs::read(&ini).unwrap().contains(&0));
        assert!(dir.join("livedelay.ini.broken").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn repair_rewrites_the_file_and_keeps_a_backup() {
        let dir = std::env::temp_dir().join(format!("livedelay-ini-repair-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("livedelay.ini");
        let original = "\
delay = x
[dest Twitch]
url = rtmp://waw.example/app
key = live_key
server = auto
";
        std::fs::write(&ini, original).unwrap();
        let c = load(&ini).unwrap();
        assert_eq!(c.delay_s, 15.0);
        assert_eq!(c.dests[0].server, "auto");
        assert_eq!(c.dests[0].key, "live_key");
        let now = std::fs::read_to_string(&ini).unwrap();
        assert!(!now.contains("delay = x"));
        assert!(now.contains("server = auto"));
        assert_eq!(std::fs::read_to_string(dir.join("livedelay.ini.bak")).unwrap(), original);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
