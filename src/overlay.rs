//! Overlay look and texts: one JSON document (`overlay.json` next to the ini),
//! read and written by the settings window and the /edit-theme page through the
//! API. Only the overlay page interprets it, so the proxy keeps it as opaque text
//! and changing it never restarts anything.

use crate::config::Overlay;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

const MAX_LEN: usize = 64 * 1024;

pub struct OverlayStore {
    json: Mutex<String>,
    path: PathBuf,
    rev: AtomicU64,
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
        Self { json: Mutex::new(json), path, rev: AtomicU64::new(1) }
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
}
