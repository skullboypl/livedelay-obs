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
# Codec stored in the delay buffer: obs | h264 | hevc | av1.
# obs keeps the OBS Output encoder and does not open a second NVENC codec.
buffer_codec = obs
# yes = OBS encodes once into the buffer (lighter FPS on a single gaming PC).
single_pc = no
# yes = the stream OBS is already sending fills the preview and the kb/s meter.
# No second encoder. Use this when From OBS stays at 0 kb/s.
obs_live = no

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

/// One RTMP feed coming in. It owns a delay engine and a buffer, and any
/// number of platforms can be fed from it, which is the whole point: OBS
/// encodes and uploads a picture once, not once per platform.
#[derive(Clone, Debug, Default)]
pub struct Input {
    pub name: String,
    /// RTMP publish key into this proxy (`ld0`, …). Empty takes the next free one.
    pub key: String,
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
    /// OBS service name (`Twitch`, `Kick`, …). Empty means a custom server.
    pub service: String,
    pub enabled: bool,
    /// Start sending as soon as OBS streams; otherwise wait for Start in LiveDelay.
    pub autostart: bool,
    /// RTMP publish key into this proxy (`ld0`, …). Empty or duplicated keys take the next free `ldN`.
    pub ingest_key: String,
    /// Name of the input feeding this platform. Empty in a file written
    /// before inputs existed, and filled in from the platform itself.
    pub input: String,
    pub encode_mode: EncodeMode,
    pub video_kbps: u32,
    pub audio_kbps: u32,
    pub keyint_sec: u32,
    pub rate_control: String,
    pub width: u32,
    pub height: u32,
    pub vod_audio: bool,
    pub enhanced_broadcasting: bool,
    /// 0 = whatever OBS uses for the stream. 1–6 = that mixer.
    pub audio_track: i32,
    /// 0 = no Twitch VOD track. 1–6 = that mixer is the second AAC.
    pub vod_track: i32,
    /// User asked to send a VOD track to a platform that is not expected to use it.
    pub vod_override: bool,
}

impl Default for Dest {
    fn default() -> Self {
        Self {
            name: String::new(),
            url: String::new(),
            server: String::new(),
            key: String::new(),
            service: String::new(),
            enabled: true,
            autostart: true,
            ingest_key: String::new(),
            input: String::new(),
            encode_mode: EncodeMode::Obs,
            video_kbps: 0,
            audio_kbps: 0,
            keyint_sec: 0,
            rate_control: "CBR".into(),
            width: 0,
            height: 0,
            vod_audio: false,
            enhanced_broadcasting: false,
            audio_track: 0,
            vod_track: 0,
            vod_override: false,
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
    /// `obs`, `h264`, `hevc` or `av1`. `obs` does not retarget NVENC.
    pub buffer_codec: String,
    /// One OBS encode fills the buffer. The plugin does not start another encoder.
    pub single_pc: bool,
    /// Accept OBS's stock publish as the preview stream. No second encoder.
    pub obs_live: bool,
    /// Declared inputs. Never empty once loaded: a file without any is given
    /// one per platform, which is exactly how the proxy behaved before.
    pub inputs: Vec<Input>,
    /// `together` (as before) or `per_input`. The engine does not read this;
    /// it tells the interfaces whether to show one set of buttons or one per input.
    pub delay_mode: String,
    pub dests: Vec<Dest>,
    /// (action, key combo) pairs for global hotkeys.
    pub hotkeys: Vec<(&'static str, String)>,
    pub overlay: Overlay,
    /// overlay.json next to the ini; empty when the config came from a string.
    pub overlay_file: std::path::PathBuf,
    /// The ini this config was loaded from. Empty when it came from a string.
    pub ini_path: std::path::PathBuf,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            listen: "127.0.0.1:1935".into(),
            api: "127.0.0.1:7799".into(),
            delay_s: 15.0,
            max_buffer_mb: 512,
            buffer_codec: "obs".into(),
            single_pc: false,
            obs_live: false,
            inputs: vec![],
            delay_mode: "together".into(),
            dests: vec![],
            hotkeys: vec![],
            overlay: Overlay::default(),
            overlay_file: Default::default(),
            ini_path: Default::default(),
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

/// `[dest Twitch]` with `word` = "dest" gives "Twitch"; `[destiny]` gives None,
/// so a section that merely starts with the word is not mistaken for one.
fn section_name<'a>(sec: &'a str, word: &str) -> Option<&'a str> {
    let rest = sec.strip_prefix(word)?;
    if rest.is_empty() || rest.starts_with(char::is_whitespace) {
        Some(rest.trim())
    } else {
        None
    }
}

/// Which section the reader is inside. `Unknown` is the one that matters: a
/// section from a newer build is copied out untouched, and its keys are not
/// applied to anything, because reading them as global settings would quietly
/// change the file's meaning.
enum Section {
    Global,
    Dest,
    Input,
    Unknown,
}

/// Reads the ini. A bad line is skipped and listed in `dropped`; unknown keys
/// and unknown sections stay in `repaired` so a newer or older build does not
/// wipe them.
pub fn parse_report(text: &str) -> ParseReport {
    let text = text.trim_start_matches('\u{feff}');
    let mut c = Config::default();
    let mut dropped = Vec::new();
    let mut kept: Vec<&str> = Vec::new();
    let mut section = Section::Global;
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        let note = |m: &str| format!("livedelay.ini, line {}: {m}", n + 1);
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            kept.push(raw);
            continue;
        }
        if line.starts_with('[') {
            let sec = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')).map(str::trim);
            match sec {
                Some(s) if section_name(s, "dest").is_some() => {
                    section = Section::Dest;
                    let name = section_name(s, "dest").unwrap_or_default();
                    c.dests.push(Dest { name: name.to_string(), ..Dest::default() });
                }
                Some(s) if section_name(s, "input").is_some() => {
                    section = Section::Input;
                    let name = section_name(s, "input").unwrap_or_default();
                    c.inputs.push(Input { name: name.to_string(), ..Input::default() });
                }
                // Not ours to judge: a section this build does not know is kept
                // word for word, exactly as unknown keys inside [dest] are.
                _ => section = Section::Unknown,
            }
            kept.push(raw);
            continue;
        }
        // Inside a section we do not understand nothing is applied, and every
        // line survives — including ones we could not parse, since we cannot
        // tell a typo from a key a later build will add.
        if matches!(section, Section::Unknown) {
            kept.push(raw);
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            dropped.push(note("brak znaku ="));
            continue;
        };
        let (k, v) = (k.trim(), v.trim());
        let mut keep = true;
        if let Section::Input = section {
            if let Some(inp) = c.inputs.last_mut() {
                if k == "key" {
                    inp.key = v.into();
                }
            }
            kept.push(raw);
            continue;
        }
        if matches!(section, Section::Dest) {
            if let Some(d) = c.dests.last_mut() {
                match k {
                    "url" => d.url = v.into(),
                    "server" => d.server = v.into(),
                    "key" => d.key = v.into(),
                    "service" => d.service = v.into(),
                    "enabled" => d.enabled = yes(v),
                    "autostart" => d.autostart = yes(v),
                    "ingest_key" => d.ingest_key = v.into(),
                    "input" => d.input = v.into(),
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
                    "audio_track" => match v.parse() {
                        Ok(n) => d.audio_track = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("audio_track must be a number"));
                        }
                    },
                    "vod_track" => match v.parse() {
                        Ok(n) => d.vod_track = n,
                        Err(_) => {
                            keep = false;
                            dropped.push(note("vod_track must be a number"));
                        }
                    },
                    "vod_override" => d.vod_override = yes(v),
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
                "buffer_codec" => match v {
                    "obs" | "h264" | "hevc" | "av1" => c.buffer_codec = v.into(),
                    _ => {
                        keep = false;
                        dropped.push(note("buffer_codec must be obs, h264, hevc or av1"));
                    }
                },
                "single_pc" => c.single_pc = yes(v),
                "delay_mode" => match v {
                    "together" | "per_input" => c.delay_mode = v.into(),
                    _ => {
                        keep = false;
                        dropped.push(note("delay_mode must be together or per_input"));
                    }
                },
                "obs_live" => c.obs_live = yes(v),
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
    assign_ingest_keys(&mut c.dests);
    finish_inputs(&mut c, &mut dropped);
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
    c.ini_path = path.to_path_buf();
    c
}

/// Fields the web panel may change. `None` leaves that line as it is.
/// When `replace_dests` is set, `dests` is the whole list: extra blocks are
/// dropped and missing ones are appended. Unknown keys inside a block that
/// stays are still copied through.
#[derive(Default)]
pub struct IniUpdate {
    pub delay: Option<String>,
    pub max_buffer_mb: Option<String>,
    pub buffer_codec: Option<String>,
    pub single_pc: Option<bool>,
    pub obs_live: Option<bool>,
    pub listen: Option<String>,
    pub api: Option<String>,
    pub language: Option<String>,
    /// `together` or `per_input`; the engine only carries it for the interfaces.
    pub delay_mode: Option<String>,
    /// Look of the OBS dock: "obs", "dark" or "contrast". The engine only
    /// carries it; the plugin is what reads it.
    pub dock_theme: Option<String>,
    pub hotkey_arm: Option<String>,
    pub hotkey_activate: Option<String>,
    pub hotkey_cut: Option<String>,
    pub hotkey_cut_after: Option<String>,
    pub arm_slots: [Option<String>; 6],
    /// True when the caller sent a whole input list and the file should end up
    /// with exactly those.
    pub replace_inputs: bool,
    pub inputs: Vec<InputUpdate>,
    pub replace_dests: bool,
    pub dests: Vec<DestUpdate>,
}

#[derive(Default)]
pub struct InputUpdate {
    pub name: Option<String>,
    pub key: Option<String>,
}

#[derive(Default)]
pub struct DestUpdate {
    pub name: Option<String>,
    /// Name of the input feeding this platform.
    pub input: Option<String>,
    pub service: Option<String>,
    pub server: Option<String>,
    pub url: Option<String>,
    pub key: Option<String>,
    pub enabled: Option<bool>,
    pub autostart: Option<bool>,
    pub encode_mode: Option<String>,
    pub video_kbps: Option<String>,
    pub audio_kbps: Option<String>,
    pub keyint_sec: Option<String>,
    pub rate_control: Option<String>,
    pub width: Option<String>,
    pub height: Option<String>,
    pub audio_track: Option<String>,
    pub vod_track: Option<String>,
    pub vod_override: Option<bool>,
    pub enhanced: Option<bool>,
}

fn split_key(line: &str) -> Option<(String, String)> {
    let t = line.trim();
    if t.is_empty() || t.starts_with('#') || t.starts_with(';') || t.starts_with('[') {
        return None;
    }
    let (k, v) = t.split_once('=')?;
    let k = k.trim();
    if k.is_empty() {
        return None;
    }
    Some((k.to_string(), v.trim().to_string()))
}

fn set_line(lines: &mut Vec<String>, key: &str, value: &str) {
    for line in lines.iter_mut() {
        if let Some((k, _)) = split_key(line) {
            if k == key {
                *line = format!("{key} = {value}");
                return;
            }
        }
    }
    let mut at = lines.len();
    while at > 0 && lines[at - 1].trim().is_empty() {
        at -= 1;
    }
    lines.insert(at, format!("{key} = {value}"));
}

fn ini_value(lines: &[String], key: &str) -> String {
    for line in lines {
        let t = line.trim();
        if t.is_empty() || t.starts_with('#') || t.starts_with(';') {
            continue;
        }
        if let Some((k, v)) = t.split_once('=') {
            if k.trim() == key {
                return v.trim().to_string();
            }
        }
    }
    String::new()
}

fn next_ld_key(dests: &[(String, Vec<String>)]) -> String {
    let mut n = 0;
    loop {
        let key = format!("ld{n}");
        if !dests.iter().any(|(_, lines)| ini_value(lines, "ingest_key") == key) {
            return key;
        }
        n += 1;
    }
}

/// Gives every input a unique publish key, then ties each platform to one.
///
/// A file written before inputs existed has none, so each platform gets one of
/// its own carrying its name and its key: one slot per platform, which is what
/// the proxy did before and must keep doing.
fn finish_inputs(c: &mut Config, dropped: &mut Vec<String>) {
    if c.inputs.is_empty() {
        for d in &c.dests {
            c.inputs.push(Input { name: d.name.clone(), key: d.ingest_key.clone() });
        }
        let names: Vec<String> = c.inputs.iter().map(|i| i.name.clone()).collect();
        for (d, name) in c.dests.iter_mut().zip(names) {
            d.input = name;
        }
    }
    let mut used: Vec<String> = Vec::new();
    for (i, inp) in c.inputs.iter_mut().enumerate() {
        if inp.key.is_empty() || used.contains(&inp.key) {
            let mut n = i;
            loop {
                let key = format!("ld{n}");
                n += 1;
                if !used.contains(&key) {
                    inp.key = key;
                    break;
                }
            }
        }
        used.push(inp.key.clone());
    }
    // A platform pointing at an input that is not there is a mistake worth
    // saying out loud, rather than quietly feeding it the first one.
    let names: Vec<String> = c.inputs.iter().map(|i| i.name.clone()).collect();
    let first = names.first().cloned().unwrap_or_default();
    for d in c.dests.iter_mut() {
        if d.input.is_empty() {
            d.input = first.clone();
        } else if !names.contains(&d.input) {
            dropped.push(format!("livedelay.ini: platform {} points at input {}, which is not there", d.name, d.input));
            d.input = first.clone();
        }
    }
}

fn assign_ingest_keys(dests: &mut [Dest]) {
    let mut used = Vec::new();
    for d in dests.iter_mut() {
        if d.ingest_key.is_empty() || used.iter().any(|k| k == &d.ingest_key) {
            let mut n = 0;
            loop {
                let key = format!("ld{n}");
                n += 1;
                if !used.iter().any(|k| k == &key) {
                    d.ingest_key = key;
                    break;
                }
            }
        }
        used.push(d.ingest_key.clone());
    }
}

/// Rewrites the ini text. Comments, unknown keys and every platform block the
/// patch does not mention are copied through.
pub fn update_ini(text: &str, upd: &IniUpdate) -> String {
    let eol = if text.contains("\r\n") { "\r\n" } else { "\n" };
    let mut global = Vec::new();
    let mut inputs: Vec<(String, Vec<String>)> = Vec::new();
    let mut dests: Vec<(String, Vec<String>)> = Vec::new();
    // A section we do not know is held apart and written back at the end,
    // untouched. Keeping it out of the three lists above is what stops a
    // setter from finding one of its keys and overwriting the wrong line.
    // Section order carries no meaning here, so moving it costs nothing.
    let mut extras: Vec<String> = Vec::new();
    enum At {
        Global,
        Input,
        Dest,
        Extra,
    }
    let mut at = At::Global;
    for raw in text.split('\n') {
        let line = raw.strip_suffix('\r').unwrap_or(raw).to_string();
        let t = line.trim();
        if t.starts_with('[') && t.ends_with(']') {
            let sec = t.trim_start_matches('[').trim_end_matches(']').trim();
            if section_name(sec, "dest").is_some() {
                at = At::Dest;
                dests.push((line, Vec::new()));
                continue;
            }
            if section_name(sec, "input").is_some() {
                at = At::Input;
                inputs.push((line, Vec::new()));
                continue;
            }
            at = At::Extra;
            extras.push(line);
            continue;
        }
        match at {
            At::Dest => dests.last_mut().expect("a dest block was opened").1.push(line),
            At::Input => inputs.last_mut().expect("an input block was opened").1.push(line),
            At::Extra => extras.push(line),
            At::Global => global.push(line),
        }
    }
    let setg = |lines: &mut Vec<String>, key: &str, value: &Option<String>| {
        if let Some(v) = value {
            set_line(lines, key, v);
        }
    };
    setg(&mut global, "delay", &upd.delay);
    setg(&mut global, "max_buffer_mb", &upd.max_buffer_mb);
    setg(&mut global, "buffer_codec", &upd.buffer_codec);
    if let Some(on) = upd.single_pc {
        set_line(&mut global, "single_pc", if on { "yes" } else { "no" });
    }
    if let Some(on) = upd.obs_live {
        set_line(&mut global, "obs_live", if on { "yes" } else { "no" });
    }
    setg(&mut global, "listen", &upd.listen);
    setg(&mut global, "api", &upd.api);
    setg(&mut global, "hotkey_arm", &upd.hotkey_arm);
    setg(&mut global, "hotkey_activate", &upd.hotkey_activate);
    setg(&mut global, "hotkey_cut", &upd.hotkey_cut);
    setg(&mut global, "hotkey_cut_after", &upd.hotkey_cut_after);
    setg(&mut global, "language", &upd.language);
    setg(&mut global, "delay_mode", &upd.delay_mode);
    setg(&mut global, "dock_theme", &upd.dock_theme);
    for i in 0..6 {
        setg(&mut global, &format!("arm_slot_{i}"), &upd.arm_slots[i]);
    }

    let apply = |lines: &mut Vec<String>, patch: &DestUpdate| {
        let set = |lines: &mut Vec<String>, key: &str, value: &Option<String>| {
            if let Some(v) = value {
                set_line(lines, key, v);
            }
        };
        set(lines, "input", &patch.input);
        set(lines, "service", &patch.service);
        set(lines, "server", &patch.server);
        set(lines, "url", &patch.url);
        set(lines, "key", &patch.key);
        set(lines, "encode_mode", &patch.encode_mode);
        set(lines, "video_kbps", &patch.video_kbps);
        set(lines, "audio_kbps", &patch.audio_kbps);
        set(lines, "keyint_sec", &patch.keyint_sec);
        set(lines, "rate_control", &patch.rate_control);
        set(lines, "width", &patch.width);
        set(lines, "height", &patch.height);
        set(lines, "audio_track", &patch.audio_track);
        if let Some(v) = &patch.vod_track {
            set_line(lines, "vod_track", v);
            let on = v != "0" && !v.is_empty();
            set_line(lines, "vod_audio", if on { "yes" } else { "no" });
        }
        if let Some(on) = patch.vod_override {
            set_line(lines, "vod_override", if on { "yes" } else { "no" });
        }
        if let Some(on) = patch.enabled {
            set_line(lines, "enabled", if on { "yes" } else { "no" });
        }
        if let Some(on) = patch.autostart {
            set_line(lines, "autostart", if on { "yes" } else { "no" });
        }
        if let Some(on) = patch.enhanced {
            set_line(lines, "enhanced_broadcasting", if on { "yes" } else { "no" });
        }
    };

    if upd.replace_inputs {
        inputs.truncate(upd.inputs.len());
    }
    for (i, patch) in upd.inputs.iter().enumerate() {
        if i >= inputs.len() {
            if !upd.replace_inputs {
                break;
            }
            let name = patch.name.clone().filter(|s| !s.is_empty()).unwrap_or_else(|| format!("Wejscie {}", i + 1));
            let mut lines = Vec::new();
            if let Some(k) = &patch.key {
                set_line(&mut lines, "key", k);
            }
            inputs.push((format!("[input {name}]"), lines));
            continue;
        }
        let (header, lines) = &mut inputs[i];
        if let Some(name) = &patch.name {
            let name = name.trim();
            if !name.is_empty() {
                *header = format!("[input {name}]");
            }
        }
        if let Some(k) = &patch.key {
            set_line(lines, "key", k);
        }
    }

    if upd.replace_dests {
        dests.truncate(upd.dests.len());
    }
    for (i, patch) in upd.dests.iter().enumerate() {
        if i >= dests.len() {
            if !upd.replace_dests {
                break;
            }
            let name = patch.name.clone().filter(|s| !s.is_empty()).unwrap_or_else(|| format!("Platforma {}", i + 1));
            let mut lines = Vec::new();
            apply(&mut lines, patch);
            // Only when there are no inputs: with them, the key belongs to the
            // input, and one written here would contradict it.
            if !upd.replace_inputs {
                set_line(&mut lines, "ingest_key", &next_ld_key(&dests));
            }
            dests.push((format!("[dest {name}]"), lines));
            continue;
        }
        let (header, lines) = &mut dests[i];
        if let Some(name) = &patch.name {
            let name = name.trim();
            if !name.is_empty() {
                *header = format!("[dest {name}]");
            }
        }
        apply(lines, patch);
    }

    let mut out = String::new();
    for line in &global {
        out.push_str(line);
        out.push_str(eol);
    }
    for (header, lines) in inputs.iter().chain(dests.iter()) {
        if !out.is_empty() && !out.ends_with(&format!("{eol}{eol}")) {
            out.push_str(eol);
        }
        out.push_str(header);
        out.push_str(eol);
        for line in lines {
            out.push_str(line);
            out.push_str(eol);
        }
    }
    if !extras.is_empty() && !out.ends_with(&format!("{eol}{eol}")) {
        out.push_str(eol);
    }
    for line in &extras {
        out.push_str(line);
        out.push_str(eol);
    }
    if !text.ends_with('\n') && out.ends_with(eol) {
        out.truncate(out.len() - eol.len());
    }
    out
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
        assert_eq!(c.buffer_codec, "obs");
        assert!(!c.single_pc);
        assert!(!c.obs_live);
        assert_eq!(c.dests.len(), 1);
        assert!(!c.dests[0].enabled);
        assert_eq!(c.dests[0].encode_mode, EncodeMode::Obs);
        assert_eq!(c.dests[0].ingest_key, "ld0");
    }

    #[test]
    fn duplicate_ingest_keys_take_the_next_free_one() {
        let text = "\
[dest Twitch]
url = rtmp://live.twitch.tv/app
ingest_key = ld0

[dest Kick]
url = rtmps://abc.global-contribute.live-video.net/app
ingest_key = ld2

[dest YouTube]
url = rtmp://a.rtmp.youtube.com/live2
ingest_key = ld2
";
        let c = parse(text).unwrap();
        assert_eq!(c.dests[0].ingest_key, "ld0");
        assert_eq!(c.dests[1].ingest_key, "ld2");
        assert_eq!(c.dests[2].ingest_key, "ld1");
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
    fn update_ini_writes_inputs_and_leaves_a_strange_section_alone() {
        let text = "delay = 15\n[scenes Vertical]\ncos = 1\n\n[dest Twitch]\nurl = rtmp://a/app\nkey = k\n";
        let upd = IniUpdate {
            replace_inputs: true,
            inputs: vec![
                InputUpdate { name: Some("Poziome".into()), key: Some("ld0".into()) },
                InputUpdate { name: Some("Pionowe".into()), key: Some("ld1".into()) },
            ],
            ..IniUpdate::default()
        };
        let out = update_ini(text, &upd);
        assert!(out.contains("[input Poziome]"));
        assert!(out.contains("[input Pionowe]"));
        // The section we do not know comes back whole, keys and all.
        assert!(out.contains("[scenes Vertical]"));
        assert!(out.contains("cos = 1"));
        // And reading it back gives what was written.
        let c = parse(&out).unwrap();
        assert_eq!(c.inputs.len(), 2);
        assert_eq!(c.inputs[1].key, "ld1");
        assert_eq!(c.delay_s, 15.0);
    }

    #[test]
    fn a_file_without_inputs_gets_one_per_platform() {
        let c = parse("[dest Twitch]
url = rtmp://a/app
key = k
[dest Kick]
url = rtmp://b/app
key = k2
").unwrap();
        assert_eq!(c.inputs.len(), 2);
        assert_eq!(c.inputs[0].name, "Twitch");
        assert_eq!(c.inputs[0].key, "ld0");
        assert_eq!(c.inputs[1].key, "ld1");
        // Each platform keeps its own feed, exactly as before inputs existed.
        assert_eq!(c.dests[0].input, "Twitch");
        assert_eq!(c.dests[1].input, "Kick");
    }

    #[test]
    fn platforms_share_an_input_by_name() {
        let text = "[input Poziome]
key = ld0
[input Pionowe]
key = ld1
                    [dest Twitch]
input = Poziome
url = rtmp://a/app
key = k
                    [dest TikTok]
input = Pionowe
url = rtmp://b/app
key = k2
";
        let c = parse(text).unwrap();
        assert_eq!(c.inputs.len(), 2);
        assert_eq!(c.dests[0].input, "Poziome");
        assert_eq!(c.dests[1].input, "Pionowe");
    }

    #[test]
    fn an_unknown_section_survives_and_stays_out_of_the_globals() {
        // A section from a later build. Its keys must not become global
        // settings, and the whole block must come back out untouched.
        let text = "delay = 15
[scenes Vertical]
delay = 20
name = TikTok
[dest Twitch]
url = rtmp://a/app
key = k
";
        let report = parse_report(text);
        assert_eq!(report.config.delay_s, 15.0, "an unknown section changed a global setting");
        assert!(report.repaired.contains("[scenes Vertical]"));
        assert!(report.repaired.contains("name = TikTok"));
        assert!(report.dropped.is_empty(), "an unknown section must not count as damage");
    }

    #[test]
    fn a_section_merely_starting_with_a_known_word_is_left_alone() {
        let report = parse_report("[inputs]
key = ld9
[destiny]
url = x
");
        assert!(report.config.inputs.is_empty());
        assert!(report.config.dests.is_empty());
        assert!(report.repaired.contains("[inputs]"));
        assert!(report.repaired.contains("[destiny]"));
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

    #[test]
    fn update_ini_keeps_comments_and_encoder_keys() {
        let src = "# keep me\r\ndelay = 15\r\nhotkey_arm =\r\n\r\n[dest Twitch]\r\nurl = rtmp://old/app\r\nkey = secret\r\nenabled = yes\r\nencode_mode = obs\r\n";
        let upd = IniUpdate {
            delay: Some("9".into()),
            max_buffer_mb: Some("640".into()),
            hotkey_arm: Some("Ctrl+Alt+F13".into()),
            dests: vec![DestUpdate {
                url: Some("rtmp://new/app".into()),
                key: Some("next".into()),
                autostart: Some(false),
                ..DestUpdate::default()
            }],
            ..IniUpdate::default()
        };
        let out = update_ini(src, &upd);
        assert!(out.contains("# keep me"));
        assert!(out.contains("delay = 9"));
        assert!(out.contains("max_buffer_mb = 640"));
        assert!(out.contains("hotkey_arm = Ctrl+Alt+F13"));
        assert!(out.contains("enabled = yes"));
        assert!(out.contains("encode_mode = obs"));
        assert!(out.contains("url = rtmp://new/app"));
        assert!(out.contains("key = next"));
        assert!(out.contains("autostart = no"));
        assert!(!out.contains("secret"));
    }
}
