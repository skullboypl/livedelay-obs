//! `livedelay.ini`: a few `key = value` lines and one `[dest NAME]` section per destination.

use std::path::Path;

pub const EXAMPLE: &str = "\
# LiveDelay - ustawienia
# W OBS: Ustawienia -> Transmisja -> Usługa: Własna, Serwer: rtmp://127.0.0.1:1935/live, Klucz: dowolny

listen = 127.0.0.1:1935
api = 127.0.0.1:7799
# opóźnienie w sekundach używane przez Arm
delay = 15
# limit pamięci na bufor (MB). 15 s przy 8 Mb/s to ok. 15 MB.
max_buffer_mb = 512

# Jeden blok na platformę. enabled = no wyłącza cel bez kasowania.
[dest Twitch]
url = rtmp://waw.contribute.live-video.net/app
key = TU_WKLEJ_KLUCZ
enabled = no
";

#[derive(Clone, Debug)]
pub struct Dest {
    pub name: String,
    pub url: String,
    pub key: String,
    pub enabled: bool,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub listen: String,
    pub api: String,
    pub delay_s: f64,
    pub max_buffer_mb: usize,
    pub dests: Vec<Dest>,
}

impl Default for Config {
    fn default() -> Self {
        Self { listen: "127.0.0.1:1935".into(), api: "127.0.0.1:7799".into(), delay_s: 15.0, max_buffer_mb: 512, dests: vec![] }
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
        let err = |m: &str| format!("livedelay.ini, linia {}: {m}", n + 1);
        if let Some(sec) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            let name = sec.trim().strip_prefix("dest").map(str::trim).ok_or_else(|| err("sekcja musi wyglądać tak: [dest Nazwa]"))?;
            c.dests.push(Dest { name: name.to_string(), url: String::new(), key: String::new(), enabled: true });
            continue;
        }
        let (k, v) = line.split_once('=').ok_or_else(|| err("brak znaku ="))?;
        let (k, v) = (k.trim(), v.trim());
        if let Some(d) = c.dests.last_mut() {
            match k {
                "url" => d.url = v.into(),
                "key" => d.key = v.into(),
                "enabled" => d.enabled = yes(v),
                _ => return Err(err(&format!("nieznane pole {k}"))),
            }
            continue;
        }
        match k {
            "listen" => c.listen = v.into(),
            "api" => c.api = v.into(),
            "delay" => c.delay_s = v.parse().map_err(|_| err("delay musi być liczbą"))?,
            "max_buffer_mb" => c.max_buffer_mb = v.parse().map_err(|_| err("max_buffer_mb musi być liczbą"))?,
            _ => return Err(err(&format!("nieznane pole {k}"))),
        }
    }
    Ok(c)
}

/// Loads the config, writing the commented example on first run.
pub fn load(path: &Path) -> Result<Config, String> {
    if !path.exists() {
        std::fs::write(path, EXAMPLE).map_err(|e| format!("nie mogę zapisać {}: {e}", path.display()))?;
    }
    let text = std::fs::read_to_string(path).map_err(|e| format!("nie mogę odczytać {}: {e}", path.display()))?;
    parse(&text)
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
        assert!(parse("delay = x").unwrap_err().contains("linia 1"));
    }
}
