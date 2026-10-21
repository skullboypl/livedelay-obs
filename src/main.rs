//! Standalone LiveDelay with a tray icon (the OBS plugin embeds the same proxy instead).
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

macro_rules! log {
    ($($t:tt)*) => { livedelay::log_line(format_args!($($t)*)) };
}

#[cfg(windows)]
mod tray;

use livedelay::{config, Proxy};
use std::path::PathBuf;

fn fatal(msg: &str) -> ! {
    log!("{msg}");
    #[cfg(windows)]
    tray::message_box(msg);
    std::process::exit(1);
}

/// Command-line path, else a portable ini next to the exe, else %APPDATA%\LiveDelay.
fn config_path() -> PathBuf {
    if let Some(p) = std::env::args().nth(1) {
        return PathBuf::from(p);
    }
    let portable = std::env::current_exe().unwrap_or_default().with_file_name("livedelay.ini");
    match std::env::var_os("APPDATA") {
        Some(appdata) if !portable.exists() => {
            let dir = PathBuf::from(appdata).join("LiveDelay");
            let _ = std::fs::create_dir_all(&dir);
            dir.join("livedelay.ini")
        }
        _ => portable,
    }
}

fn main() {
    let path = config_path();
    livedelay::set_log_file(&path.with_file_name("livedelay.log"));
    let cfg = config::load(&path).unwrap_or_else(|e| fatal(&e));
    log!("settings: {}", path.display());

    let proxy = Proxy::start(&cfg, |e| fatal(&e), |_shared| {
        #[cfg(windows)]
        tokio::spawn(tray::watch_state(_shared.clone()));
    });

    #[cfg(windows)]
    {
        tray::run(proxy.ctx.clone(), path, &cfg.hotkeys);
        // Quit from the tray: exit without waiting for open connections.
        std::process::exit(0);
    }
    #[cfg(not(windows))]
    {
        let _ = proxy;
        loop {
            std::thread::park();
        }
    }
}
