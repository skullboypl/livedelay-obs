//! LiveDelay: an RTMP proxy that puts a switchable delay between OBS and the platforms.
//! Used both by the standalone tray app (`main.rs`) and, as a static library, by the OBS plugin.

macro_rules! log {
    ($($t:tt)*) => { $crate::log_line(format_args!($($t)*)) };
}

pub mod api;
pub mod config;
pub mod engine;
mod ffi;
pub mod hub;
pub mod overlay;
mod preview;
pub mod rtmp;

use std::io::Write;
use std::path::Path;
use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;
use tokio::sync::oneshot;

type Logger = Box<dyn Fn(&str) + Send + Sync>;
static LOGGER: RwLock<Option<Logger>> = RwLock::new(None);
static LOG_FILE: Mutex<Option<std::fs::File>> = Mutex::new(None);

pub fn set_logger(f: impl Fn(&str) + Send + Sync + 'static) {
    *LOGGER.write().unwrap_or_else(|e| e.into_inner()) = Some(Box::new(f));
}

pub fn set_log_file(path: &Path) {
    *LOG_FILE.lock().unwrap_or_else(|e| e.into_inner()) = std::fs::File::create(path).ok();
}

#[cfg(windows)]
fn local_time() -> (u16, u16, u16) {
    let mut t = unsafe { std::mem::zeroed() };
    unsafe { windows_sys::Win32::System::SystemInformation::GetLocalTime(&mut t) };
    (t.wHour, t.wMinute, t.wSecond)
}

#[cfg(not(windows))]
fn local_time() -> (u16, u16, u16) {
    let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    ((s / 3600 % 24) as u16, (s / 60 % 60) as u16, (s % 60) as u16)
}

pub fn log_line(args: std::fmt::Arguments) {
    let msg = args.to_string();
    if let Some(f) = LOGGER.read().unwrap_or_else(|e| e.into_inner()).as_ref() {
        f(&msg);
        return;
    }
    let (h, m, s) = local_time();
    let line = format!("[{h:02}:{m:02}:{s:02}] {msg}");
    eprintln!("{line}");
    if let Some(f) = LOG_FILE.lock().unwrap_or_else(|e| e.into_inner()).as_mut() {
        let _ = writeln!(f, "{line}");
    }
}

/// A running proxy: its own thread with a single-threaded tokio runtime.
pub struct Proxy {
    pub ctx: Arc<api::Ctx>,
    stop: Option<oneshot::Sender<()>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Proxy {
    /// Starts ingest, destinations, playhead and API. `on_fatal` is called if a port
    /// cannot be opened; `extra` runs inside the runtime to spawn additional tasks.
    pub fn start(
        cfg: &config::Config,
        on_fatal: impl FnOnce(String) + Send + 'static,
        extra: impl FnOnce(&Arc<hub::Hub>) + Send + 'static,
    ) -> Proxy {
        let n = cfg.dests.len().max(1);
        let per_slot = (cfg.max_buffer_mb * 1024 * 1024) / n;
        let pairs: Vec<(String, String)> = cfg.dests.iter().map(|d| (d.name.clone(), d.ingest_key.clone())).collect();
        let hub = hub::Hub::new(&pairs, per_slot);

        let mut dests = Vec::new();
        let mut targets = Vec::new();
        for (i, d) in cfg.dests.iter().enumerate() {
            match rtmp::client::parse_url(&d.url, &d.key) {
                Ok(t) => {
                    let dest = rtmp::client::Dest::new(&d.name, d.autostart && d.enabled);
                    dests.push(dest.clone());
                    if let Some(slot) = hub.slot(i) {
                        targets.push((t, dest, slot.shared.clone()));
                    }
                }
                Err(e) => log!("{}: {e}", d.name),
            }
        }
        if dests.is_empty() {
            log!("no enabled platforms - the stream will not go anywhere");
        }
        let ctx = Arc::new(api::Ctx {
            hub: hub.clone(),
            dests,
            default_delay: std::sync::Mutex::new(Duration::from_secs_f64(cfg.delay_s)),
            overlay: overlay::OverlayStore::load(cfg.overlay_file.clone(), &cfg.overlay),
        });
        let (listen, api_addr) = (cfg.listen.clone(), cfg.api.clone());
        let (stop_tx, stop_rx) = oneshot::channel();
        let proxy_ctx = ctx.clone();
        let thread = std::thread::Builder::new()
            .name("livedelay".into())
            .spawn(move || {
                let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
                    Ok(rt) => rt,
                    Err(e) => return on_fatal(format!("tokio: {e}")),
                };
                rt.block_on(async move {
                    hub.spawn_state_forwarders();
                    for (target, dest, shared) in targets {
                        tokio::spawn(rtmp::client::run(target, shared, dest));
                    }
                    for slot in &hub.slots {
                        tokio::spawn(engine::run_pump(slot.shared.clone()));
                    }
                    extra(&hub);
                    let api = tokio::spawn(api::run(api_addr, proxy_ctx));
                    let ingest = tokio::spawn(rtmp::server::run(listen, hub));
                    let err = tokio::select! {
                        _ = stop_rx => return,
                        r = api => format!("API: {r:?}"),
                        r = ingest => format!("RTMP: {r:?}"),
                    };
                    on_fatal(format!("LiveDelay cannot run ({err}). Is the port already in use, e.g. by another copy?"));
                });
            })
            .expect("spawn proxy thread");
        Proxy { ctx, stop: Some(stop_tx), thread: Some(thread) }
    }

    pub fn stop(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        if let Some(s) = self.stop.take() {
            let _ = s.send(());
        }
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for Proxy {
    fn drop(&mut self) {
        self.shutdown();
    }
}
