#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

macro_rules! log {
    ($($t:tt)*) => { $crate::log_line(format_args!($($t)*)) };
}

mod api;
mod config;
mod engine;
mod rtmp;
#[cfg(windows)]
mod tray;

use std::io::Write;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

static LOG_FILE: OnceLock<Mutex<std::fs::File>> = OnceLock::new();

/// Logs to stderr and, since the release build has no console, to livedelay.log.
pub fn log_line(args: std::fmt::Arguments) {
    let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let line = format!("[{:02}:{:02}:{:02}] {args}", s / 3600 % 24, s / 60 % 60, s % 60);
    eprintln!("{line}");
    if let Some(f) = LOG_FILE.get() {
        let _ = writeln!(f.lock().unwrap(), "{line}");
    }
}

fn fatal(msg: &str) -> ! {
    log!("{msg}");
    #[cfg(windows)]
    tray::message_box(msg);
    std::process::exit(1);
}

fn config_path() -> PathBuf {
    if let Some(p) = std::env::args().nth(1) {
        return PathBuf::from(p);
    }
    std::env::current_exe().unwrap_or_default().with_file_name("livedelay.ini")
}

fn main() {
    let path = config_path();
    if let Ok(f) = std::fs::File::create(path.with_file_name("livedelay.log")) {
        let _ = LOG_FILE.set(Mutex::new(f));
    }
    let cfg = config::load(&path).unwrap_or_else(|e| fatal(&e));
    log!("ustawienia: {}", path.display());

    let shared = engine::Shared::new(cfg.max_buffer_mb * 1024 * 1024);
    let mut dests = Vec::new();
    let mut targets = Vec::new();
    for d in cfg.dests.iter().filter(|d| d.enabled) {
        match rtmp::client::parse_url(&d.url, &d.key) {
            Ok(t) => {
                let st = Arc::new(Mutex::new(rtmp::client::DestStatus { name: d.name.clone(), ..Default::default() }));
                dests.push(st.clone());
                targets.push((d.name.clone(), t, st));
            }
            Err(e) => log!("{}: {e}", d.name),
        }
    }
    if dests.is_empty() {
        log!("brak włączonych celów w livedelay.ini - stream nie wyjdzie dalej");
    }
    let ctx = Arc::new(api::Ctx { shared: shared.clone(), dests, default_delay: Duration::from_secs_f64(cfg.delay_s) });

    let (listen, api_addr) = (cfg.listen.clone(), cfg.api.clone());
    let proxy_ctx = ctx.clone();
    let proxy = std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio");
        rt.block_on(async move {
            for (name, target, st) in targets {
                tokio::spawn(rtmp::client::run(name, target, shared.clone(), st));
            }
            tokio::spawn(engine::run_pump(shared.clone()));
            #[cfg(windows)]
            tokio::spawn(tray::watch_state(shared.clone()));
            let api = tokio::spawn(api::run(api_addr, proxy_ctx));
            let ingest = tokio::spawn(rtmp::server::run(listen, shared));
            let err = tokio::select! {
                r = api => format!("API: {r:?}"),
                r = ingest => format!("RTMP: {r:?}"),
            };
            fatal(&format!("LiveDelay nie może działać ({err}). Czy port nie jest zajęty, np. przez drugą kopię programu?"));
        });
    });

    #[cfg(windows)]
    tray::run(ctx, path, &cfg.hotkeys);
    #[cfg(not(windows))]
    let _ = proxy.join();
    #[cfg(windows)]
    drop(proxy); // Quit from the tray ends the process, and with it the proxy thread.
}
