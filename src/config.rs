//! `livedelay.ini`: a few `key = value` lines and one `[dest NAME]` section per destination.

use std::path::Path;

pub const EXAMPLE: &str = "\
# LiveDelay settings (in OBS: Tools -> LiveDelay)

listen = 127.0.0.1:1935
api = 127.0.0.1:7799
# delay in seconds used by Arm
delay = 15
# memory limit for the buffer (MB). 15 s at 8 Mb/s is about 15 MB.
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
[dest Twitch]
url = rtmp://live.twitch.tv/app
key = PASTE_YOUR_KEY
enabled = no
autostart = yes
";

#[derive(Clone, Debug)]
pub struct Dest {
    pub name: String,
    pub url: String,
    pub key: String,
    pub enabled: bool,
    /// Start sending as soon as OBS streams; otherwise wait for Start in LiveDelay.
    pub autostart: bool,
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
        Self { listen: "127.0.0.1:1935".into(), api: "127.0.0.1:7799".into(), delay_s: 15.0, max_buffer_mb: 512, dests: vec![], hotkeys: vec![], overlay: Overlay::default(), overlay_file: Default::default() }
    }
}

fn yes(v: &str) -> bool {
    matches!(v.to_ascii_lowercase().as_str(), "yes" | "tak" | "true" | "1" | "on")
}

pub fn parse(text: &str) -> Result<Config, String> {
    let mut c = Config::default();
    for (n, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        let err = |m: &str| format!("livedelay.ini, line {}: {m}", n + 1);
        if let Some(sec) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            let name = sec.trim().strip_prefix("dest").map(str::trim).ok_or_else(|| err("a section must look like [dest Name]"))?;
            c.dests.push(Dest { name: name.to_string(), url: String::new(), key: String::new(), enabled: true, autostart: true });
            continue;
        }
        let (k, v) = line.split_once('=').ok_or_else(|| err("brak znaku ="))?;
        let (k, v) = (k.trim(), v.trim());
        if let Some(d) = c.dests.last_mut() {
            match k {
                "url" => d.url = v.into(),
                "key" => d.key = v.into(),
                "enabled" => d.enabled = yes(v),
                "autostart" => d.autostart = yes(v),
                // Which OBS service the URL came from; only the settings window uses it.
                "service" => {}
                _ => return Err(err(&format!("unknown key {k}"))),
            }
            continue;
        }
        match k {
            "listen" => c.listen = v.into(),
            "api" => c.api = v.into(),
            "delay" => c.delay_s = v.parse().map_err(|_| err("delay must be a number"))?,
            "max_buffer_mb" => c.max_buffer_mb = v.parse().map_err(|_| err("max_buffer_mb must be a number"))?,
            // Plugin UI language; the proxy itself has no texts to translate.
            "language" => {}
            "overlay_live" => c.overlay.live = v.into(),
            "overlay_arming" => c.overlay.arming = v.into(),
            "overlay_armed" => c.overlay.armed = v.into(),
            "overlay_delayed" => c.overlay.delayed = v.into(),
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
            _ => return Err(err(&format!("unknown key {k}"))),
        }
    }
    Ok(c)
}

/// Loads the config, writing the commented example on first run.
pub fn load(path: &Path) -> Result<Config, String> {
    if !path.exists() {
        std::fs::write(path, EXAMPLE).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    }
    let text = std::fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    let mut c = parse(&text)?;
    c.overlay_file = path.with_file_name("overlay.json");
    Ok(c)
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
    }

    #[test]
    fn reports_line_numbers() {
        assert!(parse("delay = x").unwrap_err().contains("line 1"));
    }
}
