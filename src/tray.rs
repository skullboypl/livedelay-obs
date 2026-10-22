//! Windows tray icon, its menu and global hotkeys. Runs the Win32 message loop
//! on the main thread; the proxy itself lives on the tokio thread.

use livedelay::api::{act, Ctx};
use livedelay::engine::Mode;
use std::path::PathBuf;
use std::sync::atomic::{AtomicIsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::Instant;
use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::*;
use windows_sys::Win32::UI::Shell::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

const WM_TRAY: u32 = WM_APP + 1;
/// Posted from the tokio thread whenever the state changes.
const WM_STATE: u32 = WM_APP + 2;

const ID_ARM: usize = 1;
const ID_ACTIVATE: usize = 2;
const ID_CUT: usize = 3;
const ID_CUT_AFTER: usize = 4;
const ID_DISARM: usize = 5;
const ID_SETTINGS: usize = 6;
const ID_QUIT: usize = 7;

struct App {
    ctx: Arc<Ctx>,
    config_path: PathBuf,
    hotkeys: Vec<&'static str>,
    /// HICONs, stored as integers so the struct can live in a static.
    icons: [isize; 3],
    taskbar_created: u32,
}

static APP: OnceLock<App> = OnceLock::new();
static TRAY_HWND: AtomicIsize = AtomicIsize::new(0);

/// Runs on the proxy's runtime: repaints the tray on every state change, and once a
/// second so the buffer fill shown in the tooltip moves.
pub async fn watch_state(hub: Arc<livedelay::hub::Hub>) {
    let mut rx = hub.state.subscribe();
    loop {
        if let Ok(Err(_)) = tokio::time::timeout(std::time::Duration::from_secs(1), rx.changed()).await {
            return;
        }
        let hwnd = TRAY_HWND.load(Ordering::SeqCst);
        if hwnd != 0 {
            unsafe { PostMessageW(hwnd as HWND, WM_STATE, 0, 0) };
        }
    }
}

pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn message_box(text: &str) {
    unsafe {
        MessageBoxW(std::ptr::null_mut(), wide(text).as_ptr(), wide("LiveDelay").as_ptr(), MB_OK | MB_ICONWARNING);
    }
}

/// A filled 16x16 circle, so the state reads at a glance: green live, amber armed, red delayed.
fn circle_icon(rgb: (u8, u8, u8)) -> HICON {
    const N: i32 = 16;
    let mut color = vec![0u32; (N * N) as usize];
    for y in 0..N {
        for x in 0..N {
            let (dx, dy) = (x as f32 - 7.5, y as f32 - 7.5);
            let d = (dx * dx + dy * dy).sqrt();
            let a = (7.6 - d).clamp(0.0, 1.0);
            let a8 = (a * 255.0) as u32;
            let pm = |c: u8| (c as f32 * a) as u32;
            color[(y * N + x) as usize] = (a8 << 24) | (pm(rgb.0) << 16) | (pm(rgb.1) << 8) | pm(rgb.2);
        }
    }
    unsafe {
        let mask = vec![0u8; (N * N / 8) as usize];
        let hbm_mask = CreateBitmap(N, N, 1, 1, mask.as_ptr().cast());
        let hbm_color = CreateBitmap(N, N, 1, 32, color.as_ptr().cast());
        let info = ICONINFO { fIcon: 1, xHotspot: 0, yHotspot: 0, hbmMask: hbm_mask, hbmColor: hbm_color };
        let icon = CreateIconIndirect(&info);
        DeleteObject(hbm_mask);
        DeleteObject(hbm_color);
        icon
    }
}

fn notify_data(hwnd: HWND) -> NOTIFYICONDATAW {
    let mut nid: NOTIFYICONDATAW = unsafe { std::mem::zeroed() };
    nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid
}

fn summary(app: &App) -> (usize, String) {
    let st = app.ctx.hub.status(Instant::now());
    let input = if st.input { "" } else { " · brak sygnału z OBS" };
    match st.mode {
        // Show configured Arm seconds, not measured keyframe lag after Activate.
        Mode::Delayed => (
            2,
            format!(
                "OPÓŹNIENIE {:.0} s{input}",
                st.arm_delay_s.unwrap_or(st.delay_s)
            ),
        ),
        Mode::Live if st.armed => (1, format!("NA ŻYWO · uzbrojone {:.0} s{input}", st.arm_delay_s.unwrap_or(0.0))),
        Mode::Live if st.arm_delay_s.is_some() => (1, format!("NA ŻYWO · bufor {:.0}/{:.0} s{input}", st.buffer_s, st.arm_delay_s.unwrap_or(0.0))),
        Mode::Live => (0, format!("NA ŻYWO{input}")),
    }
}

fn refresh(hwnd: HWND, add: bool) {
    let Some(app) = APP.get() else { return };
    let (icon, text) = summary(app);
    let mut nid = notify_data(hwnd);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = app.icons[icon] as HICON;
    let tip: Vec<u16> = format!("LiveDelay: {text}").encode_utf16().take(127).collect();
    nid.szTip[..tip.len()].copy_from_slice(&tip);
    unsafe {
        Shell_NotifyIconW(if add { NIM_ADD } else { NIM_MODIFY }, &nid);
    }
}

fn run_action(action: &str) {
    let Some(app) = APP.get() else { return };
    if let Err(e) = act(&app.ctx, action, None) {
        unsafe {
            let hwnd = FindWindowW(wide("LiveDelayTray").as_ptr(), std::ptr::null());
            let mut nid = notify_data(hwnd);
            nid.uFlags = NIF_INFO;
            let t: Vec<u16> = e.encode_utf16().take(255).collect();
            nid.szInfo[..t.len()].copy_from_slice(&t);
            let title: Vec<u16> = "LiveDelay".encode_utf16().collect();
            nid.szInfoTitle[..title.len()].copy_from_slice(&title);
            nid.dwInfoFlags = NIIF_WARNING;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
    }
}

fn show_menu(hwnd: HWND) {
    let Some(app) = APP.get() else { return };
    let st = app.ctx.hub.status(Instant::now());
    let (_, text) = summary(app);
    let default = app.ctx.default_delay.lock().unwrap_or_else(|e| e.into_inner()).as_secs_f64();
    unsafe {
        let m = CreatePopupMenu();
        let item = |id: usize, label: &str, enabled: bool| {
            let flags = MF_STRING | if enabled { MF_ENABLED } else { MF_GRAYED };
            AppendMenuW(m, flags, id, wide(label).as_ptr());
        };
        item(0, &text, false);
        AppendMenuW(m, MF_SEPARATOR, 0, std::ptr::null());
        let delayed = st.mode == Mode::Delayed;
        item(ID_ARM, &format!("Arm ({default:.0} s)"), !delayed);
        item(ID_ACTIVATE, "Activate (włącz opóźnienie)", !delayed && st.armed);
        item(ID_CUT, "Cut (wróć na żywo)", delayed);
        item(ID_CUT_AFTER, "Cut po wyemitowaniu", delayed && !st.cut_pending);
        item(ID_DISARM, "Rozbrój (zwolnij bufor)", !delayed && st.arm_delay_s.is_some());
        AppendMenuW(m, MF_SEPARATOR, 0, std::ptr::null());
        item(ID_SETTINGS, "Ustawienia (livedelay.ini)", true);
        item(ID_QUIT, "Zakończ", true);
        let mut pt = POINT { x: 0, y: 0 };
        GetCursorPos(&mut pt);
        SetForegroundWindow(hwnd);
        let cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, std::ptr::null());
        DestroyMenu(m);
        match cmd as usize {
            ID_ARM => run_action("arm"),
            ID_ACTIVATE => run_action("activate"),
            ID_CUT => run_action("cut"),
            ID_CUT_AFTER => run_action("cut-after"),
            ID_DISARM => run_action("disarm"),
            ID_SETTINGS => {
                ShellExecuteW(hwnd, wide("open").as_ptr(), wide("notepad.exe").as_ptr(), wide(&app.config_path.display().to_string()).as_ptr(), std::ptr::null(), SW_SHOWNORMAL);
            }
            ID_QUIT => {
                DestroyWindow(hwnd);
            }
            _ => {}
        }
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_TRAY => {
            let ev = (lp & 0xffff) as u32;
            if ev == WM_RBUTTONUP || ev == WM_LBUTTONUP {
                show_menu(hwnd);
            }
            0
        }
        WM_STATE => {
            refresh(hwnd, false);
            0
        }
        WM_HOTKEY => {
            if let Some(a) = APP.get().and_then(|a| a.hotkeys.get(wp)) {
                run_action(a);
            }
            0
        }
        WM_DESTROY => {
            Shell_NotifyIconW(NIM_DELETE, &notify_data(hwnd));
            PostQuitMessage(0);
            0
        }
        _ if APP.get().is_some_and(|a| a.taskbar_created == msg) => {
            refresh(hwnd, true); // Explorer restarted: put the icon back.
            0
        }
        _ => DefWindowProcW(hwnd, msg, wp, lp),
    }
}

/// Parses "Ctrl+Alt+Num1", "F13", "Shift+F5" into (modifiers, virtual key).
pub fn parse_hotkey(s: &str) -> Option<(u32, u32)> {
    let mut mods = MOD_NOREPEAT;
    let mut vk = None;
    for part in s.split('+').map(|p| p.trim().to_ascii_lowercase()) {
        match part.as_str() {
            "ctrl" | "control" => mods |= MOD_CONTROL,
            "alt" => mods |= MOD_ALT,
            "shift" => mods |= MOD_SHIFT,
            "win" => mods |= MOD_WIN,
            p => {
                let code = if let Some(n) = p.strip_prefix("num") {
                    match n {
                        "+" | "add" | "plus" => 0x6B,
                        "-" | "sub" | "minus" => 0x6D,
                        "*" | "mul" => 0x6A,
                        "/" | "div" => 0x6F,
                        "." | "dot" | "del" => 0x6E,
                        d => 0x60 + d.parse::<u32>().ok().filter(|d| *d <= 9)?,
                    }
                } else if let Some(n) = p.strip_prefix('f').and_then(|n| n.parse::<u32>().ok()) {
                    if !(1..=24).contains(&n) {
                        return None;
                    }
                    0x6F + n
                } else if p.len() == 1 && p.chars().all(|c| c.is_ascii_alphanumeric()) {
                    p.to_ascii_uppercase().as_bytes()[0] as u32
                } else {
                    match p {
                        "pause" => 0x13,
                        "scrolllock" => 0x91,
                        "insert" => 0x2D,
                        "home" => 0x24,
                        "end" => 0x23,
                        "pageup" => 0x21,
                        "pagedown" => 0x22,
                        _ => return None,
                    }
                };
                vk = Some(code);
            }
        }
    }
    vk.map(|v| (mods, v))
}

pub fn run(ctx: Arc<Ctx>, config_path: PathBuf, hotkeys: &[(&'static str, String)]) {
    unsafe {
        let hinst = GetModuleHandleW(std::ptr::null());
        let class = wide("LiveDelayTray");
        let wc = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            hInstance: hinst,
            lpszClassName: class.as_ptr(),
            ..std::mem::zeroed()
        };
        RegisterClassW(&wc);
        let hwnd = CreateWindowExW(0, class.as_ptr(), class.as_ptr(), 0, 0, 0, 0, 0, HWND_MESSAGE, std::ptr::null_mut(), hinst, std::ptr::null());

        let mut registered = Vec::new();
        for (action, combo) in hotkeys {
            match parse_hotkey(combo) {
                Some((mods, vk)) if RegisterHotKey(hwnd, registered.len() as i32, mods, vk) != 0 => {
                    log!("skrót {combo} → {action}");
                    registered.push(*action);
                }
                Some(_) => log!("skrót {combo} jest zajęty przez inny program"),
                None => log!("nie rozumiem skrótu {combo:?}"),
            }
        }
        let _ = APP.set(App {
            ctx: ctx.clone(),
            config_path,
            hotkeys: registered,
            icons: [(52, 199, 89), (255, 179, 0), (235, 64, 52)].map(|c| circle_icon(c) as isize),
            taskbar_created: RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()),
        });
        refresh(hwnd, true);

        TRAY_HWND.store(hwnd as isize, Ordering::SeqCst);

        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::parse_hotkey;

    #[test]
    fn hotkeys() {
        assert_eq!(parse_hotkey("Ctrl+Alt+Num1").map(|h| h.1), Some(0x61));
        assert_eq!(parse_hotkey("F13").map(|h| h.1), Some(0x7C));
        assert_eq!(parse_hotkey("shift+a").map(|h| h.1), Some(b'A' as u32));
        assert!(parse_hotkey("Ctrl+").is_none());
        assert!(parse_hotkey("F25").is_none());
    }
}
