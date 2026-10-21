macro_rules! log {
    ($($t:tt)*) => {{
        let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        eprintln!("[{:02}:{:02}:{:02}] {}", s / 3600 % 24, s / 60 % 60, s % 60, format_args!($($t)*));
    }};
}

mod api;
mod config;
mod engine;
mod rtmp;

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

fn config_path() -> PathBuf {
    if let Some(p) = std::env::args().nth(1) {
        return PathBuf::from(p);
    }
    let exe = std::env::current_exe().unwrap_or_default();
    exe.with_file_name("livedelay.ini")
}

fn main() {
    let path = config_path();
    let cfg = match config::load(&path) {
        Ok(c) => c,
        Err(e) => {
            log!("{e}");
            std::process::exit(1);
        }
    };
    log!("ustawienia: {}", path.display());

    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio");
    rt.block_on(async move {
        let shared = engine::Shared::new(cfg.max_buffer_mb * 1024 * 1024);
        let mut dests = Vec::new();
        for d in cfg.dests.iter().filter(|d| d.enabled) {
            let target = match rtmp::client::parse_url(&d.url, &d.key) {
                Ok(t) => t,
                Err(e) => {
                    log!("{}: {e}", d.name);
                    continue;
                }
            };
            let st = Arc::new(Mutex::new(rtmp::client::DestStatus { name: d.name.clone(), ..Default::default() }));
            dests.push(st.clone());
            tokio::spawn(rtmp::client::run(d.name.clone(), target, shared.clone(), st));
        }
        if dests.is_empty() {
            log!("brak włączonych celów w livedelay.ini - stream nie wyjdzie dalej");
        }
        let ctx = Arc::new(api::Ctx { shared: shared.clone(), dests, default_delay: Duration::from_secs_f64(cfg.delay_s) });
        tokio::spawn(engine::run_pump(shared.clone()));
        let api = tokio::spawn(api::run(cfg.api.clone(), ctx));
        let ingest = tokio::spawn(rtmp::server::run(cfg.listen.clone(), shared));
        tokio::select! {
            r = api => log!("API zatrzymane: {r:?}"),
            r = ingest => log!("RTMP zatrzymany: {r:?}"),
        }
    });
}
