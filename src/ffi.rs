//! C API of the shared engine.
//! The OBS plugin links these symbols from the staticlib (`livedelay.lib`).
//! The standalone app loads the same symbols from the cdylib (`livedelay.dll`).
//! Both files are built only in this repository.

use crate::{config, Proxy};
use std::ffi::{c_char, c_int, CStr, CString};
use std::path::Path;
use std::sync::Mutex;

static PROXY: Mutex<Option<Proxy>> = Mutex::new(None);
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());
/// Who started the engine. Empty until the host says so; the web panel uses it
/// to hide settings only the OBS plugin can act on.
static HOST: Mutex<String> = Mutex::new(String::new());

pub fn host() -> String {
    HOST.lock().unwrap_or_else(|p| p.into_inner()).clone()
}

pub type LogFn = extern "C" fn(msg: *const c_char);

/// Engine version, the same number as the OBS plugin (`Cargo.toml`).
/// The standalone app shows it apart from its own exe version.
#[no_mangle]
pub extern "C" fn ld_version() -> *const c_char {
    static VERSION: &[u8] = concat!(env!("CARGO_PKG_VERSION"), "\0").as_bytes();
    VERSION.as_ptr().cast()
}

fn set_error(e: String) {
    log!("{e}");
    *LAST_ERROR.lock().unwrap_or_else(|p| p.into_inner()) = e;
}

/// Routes LiveDelay's log lines to the host (OBS's log).
#[no_mangle]
pub extern "C" fn ld_set_logger(f: Option<LogFn>) {
    if let Some(f) = f {
        crate::set_logger(move |m| {
            if let Ok(c) = CString::new(m.replace('\0', " ")) {
                f(c.as_ptr());
            }
        });
    }
}

/// Names the host the engine runs in: "obs" from the plugin, anything else (or
/// nothing at all) from another host. Call it before `ld_start`.
///
/// # Safety
/// `name` must be a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn ld_set_host(name: *const c_char) {
    if name.is_null() {
        return;
    }
    let name = CStr::from_ptr(name).to_string_lossy().into_owned();
    *HOST.lock().unwrap_or_else(|p| p.into_inner()) = name;
}

/// Starts the proxy with the ini at `config_path` (UTF-8), stopping a running one first.
/// Returns 0 on success, 1 if the config can't be read; see `ld_last_error`.
///
/// # Safety
/// `config_path` must be a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn ld_start(config_path: *const c_char) -> c_int {
    // Keep a panic inside the Rust core from taking down OBS.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        ld_stop();
        LAST_ERROR.lock().unwrap_or_else(|p| p.into_inner()).clear();
        let path = CStr::from_ptr(config_path).to_string_lossy().into_owned();
        let cfg = match config::load(Path::new(&path)) {
            Ok(c) => c,
            Err(e) => {
                set_error(e);
                return 1;
            }
        };
        log!("settings: {path}");
        let proxy = Proxy::start(&cfg, set_error, |_| {});
        *PROXY.lock().unwrap_or_else(|p| p.into_inner()) = Some(proxy);
        0
    }));
    match result {
        Ok(code) => code,
        Err(_) => {
            set_error("internal panic while starting LiveDelay".into());
            1
        }
    }
}

/// Stops the proxy and closes all connections. Safe to call when not running.
#[no_mangle]
pub extern "C" fn ld_stop() {
    let p = PROXY.lock().unwrap_or_else(|p| p.into_inner()).take();
    if let Some(p) = p {
        p.stop();
        log!("stopped");
    }
}

/// Copies the last error (UTF-8, NUL-terminated, possibly truncated) into `buf`.
/// Returns its full length in bytes, 0 if there was none.
///
/// # Safety
/// `buf` must point to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ld_last_error(buf: *mut c_char, len: usize) -> usize {
    let e = LAST_ERROR.lock().unwrap_or_else(|p| p.into_inner()).clone();
    if len > 0 && !buf.is_null() {
        let n = e.len().min(len - 1);
        std::ptr::copy_nonoverlapping(e.as_ptr(), buf.cast(), n);
        *buf.add(n) = 0;
    }
    e.len()
}
