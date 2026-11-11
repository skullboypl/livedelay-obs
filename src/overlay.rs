//! Overlay look and texts: one JSON document (`overlay.json` next to the ini),
//! read and written by the settings window and the /edit-theme page through the
//! API. Only the overlay page interprets it, so the proxy keeps it as opaque text
//! and changing it never restarts anything.
//!
//! An input may have a look of its own (`overlays/<input>.json`): the overlay
//! is drawn into the picture, so a vertical TikTok picture can carry another
//! one than the OBS picture. An input without its own file shows the main look,
//! and asking for it says so with `"inherited":true`; posting a document that
//! says `"inherited":true` removes the input's file again.

use crate::config::Overlay;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

const MAX_LEN: usize = 64 * 1024;

pub struct OverlayStore {
    json: Mutex<String>,
    path: PathBuf,
    rev: AtomicU64,
    /// Input name → its own look, `None` once known to have none.
    inputs: Mutex<HashMap<String, Option<String>>>,
}

/// A file name for any input name: letters, digits, `-` and `_` stay, every
/// other character becomes `~` and its code, so two names never share a file.
fn file_stem(input: &str) -> String {
    let mut out = String::new();
    for c in input.chars() {
        if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
            out.push(c);
        } else {
            out.push_str(&format!("~{:x}", c as u32));
        }
    }
    out
}

fn says_inherited(body: &str) -> bool {
    body.split_whitespace().collect::<String>().contains("\"inherited\":true")
}

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

/// First-run look: the "glass" theme with the texts from the ini (older versions kept them there).
fn default_json(o: &Overlay) -> String {
    format!(
        concat!(
            "{{\"version\":1,\"theme\":\"glass\",\"align\":\"left\",\"size\":30,\"font\":\"Segoe UI\",",
            "\"uppercase\":false,\"dot\":true,\"animate\":true,\"radius\":\"pill\",\"opacity\":0.78,",
            "\"colors\":{{\"live\":\"#3ddc84\",\"arming\":\"#ffbf3c\",\"armed\":\"#ffbf3c\",\"delayed\":\"#ff5a4f\"}},",
            "\"texts\":{{\"live\":\"{}\",\"arming\":\"{}\",\"armed\":\"{}\",\"delayed\":\"{}\"}},",
            "\"show\":{{\"live\":{},\"arming\":true,\"armed\":true,\"delayed\":true}}}}"
        ),
        esc(&o.live),
        esc(&o.arming),
        esc(&o.armed),
        esc(&o.delayed),
        !o.live.is_empty()
    )
}

fn plausible(s: &str) -> bool {
    let t = s.trim();
    t.len() <= MAX_LEN && t.starts_with('{') && t.ends_with('}')
}

impl OverlayStore {
    pub fn load(path: PathBuf, defaults: &Overlay) -> Self {
        let saved = std::fs::read_to_string(&path).ok().filter(|s| plausible(s));
        let json = saved.clone().unwrap_or_else(|| default_json(defaults));
        // Write the first-run look out right away, so the ini can drop its old overlay_* lines.
        if saved.is_none() && !path.as_os_str().is_empty() {
            let _ = std::fs::write(&path, &json);
        }
        Self { json: Mutex::new(json), path, rev: AtomicU64::new(1), inputs: Mutex::new(HashMap::new()) }
    }

    fn input_path(&self, input: &str) -> Option<PathBuf> {
        if self.path.as_os_str().is_empty() {
            return None;
        }
        Some(self.path.with_file_name("overlays").join(format!("{}.json", file_stem(input))))
    }

    fn own(&self, input: &str) -> Option<String> {
        let mut map = self.inputs.lock().unwrap_or_else(|e| e.into_inner());
        map.entry(input.to_string())
            .or_insert_with(|| {
                self.input_path(input).and_then(|p| std::fs::read_to_string(p).ok()).filter(|s| plausible(s))
            })
            .clone()
    }

    /// The look one overlay shows: the input's own, or the main one marked as such.
    pub fn get_for(&self, input: Option<&str>) -> String {
        let Some(input) = input else { return self.get() };
        if let Some(own) = self.own(input) {
            return own;
        }
        let main = self.get();
        match main.trim_start().strip_prefix('{') {
            Some(rest) if rest.trim_start().starts_with('}') => "{\"inherited\":true}".into(),
            Some(rest) => format!("{{\"inherited\":true,{rest}"),
            None => main,
        }
    }

    pub fn set_for(&self, input: Option<&str>, body: &str) -> Result<(), &'static str> {
        let Some(input) = input else { return self.set(body) };
        if !plausible(body) {
            return Err("bad_overlay_config");
        }
        let path = self.input_path(input);
        let own = if says_inherited(body) { None } else { Some(body.trim().to_string()) };
        self.inputs.lock().unwrap_or_else(|e| e.into_inner()).insert(input.to_string(), own.clone());
        self.rev.fetch_add(1, Ordering::SeqCst);
        let Some(path) = path else { return Ok(()) };
        let written = match &own {
            None => match std::fs::remove_file(&path) {
                Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
                _ => Ok(()),
            },
            Some(json) => std::fs::create_dir_all(path.parent().unwrap_or(&path)).and_then(|_| {
                let tmp = path.with_extension("json.tmp");
                std::fs::write(&tmp, json).and_then(|_| std::fs::rename(&tmp, &path))
            }),
        };
        written.map_err(|e| {
            log!("overlay: cannot save {}: {e}", path.display());
            "save_failed"
        })
    }

    pub fn get(&self) -> String {
        self.json.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    pub fn rev(&self) -> u64 {
        self.rev.load(Ordering::SeqCst)
    }

    pub fn set(&self, body: &str) -> Result<(), &'static str> {
        if !plausible(body) {
            return Err("bad_overlay_config");
        }
        *self.json.lock().unwrap_or_else(|e| e.into_inner()) = body.trim().to_string();
        self.rev.fetch_add(1, Ordering::SeqCst);
        if !self.path.as_os_str().is_empty() {
            let tmp = self.path.with_extension("json.tmp");
            std::fs::write(&tmp, body.trim()).and_then(|_| std::fs::rename(&tmp, &self.path)).map_err(|e| {
                log!("overlay: cannot save {}: {e}", self.path.display());
                "save_failed"
            })?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_carries_ini_texts_and_rejects_junk() {
        let o = Overlay { live: String::new(), ..Overlay::default() };
        let s = OverlayStore::load(PathBuf::new(), &o);
        assert!(s.get().contains("\"live\":false"));
        assert!(s.get().contains("ARMED {s}s"));
        assert!(s.set("not json").is_err());
        assert!(s.set("{\"theme\":\"neon\"}").is_ok());
        assert_eq!(s.get(), "{\"theme\":\"neon\"}");
        assert_eq!(s.rev(), 2);
    }

    #[test]
    fn an_input_shows_the_main_look_until_it_has_its_own() {
        let dir = std::env::temp_dir().join(format!("ld-overlay-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let s = OverlayStore::load(dir.join("overlay.json"), &Overlay::default());
        s.set("{\"theme\":\"glass\"}").unwrap();

        assert_eq!(s.get_for(Some("Pionowe")), "{\"inherited\":true,\"theme\":\"glass\"}");
        s.set_for(Some("Pionowe"), "{\"theme\":\"neon\"}").unwrap();
        assert_eq!(s.get_for(Some("Pionowe")), "{\"theme\":\"neon\"}");
        assert_eq!(s.get_for(None), "{\"theme\":\"glass\"}");
        assert!(dir.join("overlays").join("Pionowe.json").exists());

        // A fresh store reads it back from the file.
        let again = OverlayStore::load(dir.join("overlay.json"), &Overlay::default());
        assert_eq!(again.get_for(Some("Pionowe")), "{\"theme\":\"neon\"}");

        s.set_for(Some("Pionowe"), "{\"inherited\": true, \"theme\":\"neon\"}").unwrap();
        assert!(s.get_for(Some("Pionowe")).starts_with("{\"inherited\":true"));
        assert!(!dir.join("overlays").join("Pionowe.json").exists());

        assert_eq!(file_stem("TikTok 9:16"), "TikTok~209~3a16");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
