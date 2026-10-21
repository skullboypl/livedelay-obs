//! Local HTTP control: the OBS plugin, Stream Deck and scripts talk to this.
//!
//! GET  /state            current state as JSON
//! GET  /events           the same JSON pushed (SSE) whenever it changes
//! POST /arm[?delay=15]   start filling the buffer (GET works too, for Stream Deck)
//! POST /activate         go delayed
//! POST /cut              back to live now
//! POST /cut-after        back to live once the current moment has aired
//! POST /disarm           stop keeping the buffer
//! GET  /overlay          status text for an OBS Browser source (?preview shows every state)
//! GET  /live-preview     what OBS sends vs. what viewers get, side by side
//! POST /dest/start?i=0   start / stop one destination (/dest/stop?i=0)
//! GET  /overlay/config   overlay look and texts as JSON; POST replaces it
//! GET  /edit-theme       visual editor for the overlay

use crate::engine::{Mode, Shared};
use crate::rtmp::client::Dest;
use std::fmt::Write as _;
use std::io;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

pub struct Ctx {
    pub shared: Arc<Shared>,
    pub dests: Vec<Arc<Dest>>,
    pub default_delay: Duration,
    pub overlay: crate::overlay::OverlayStore,
}

const OVERLAY_HTML: &str = include_str!("overlay.html");
const PREVIEW_HTML: &str = include_str!("preview.html");
const EDITOR_HTML: &str = include_str!("edit-theme.html");

fn esc(s: &str) -> String {
    let mut o = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => {
                let _ = write!(o, "\\u{:04x}", c as u32);
            }
            c => o.push(c),
        }
    }
    o
}

pub fn state_json(ctx: &Ctx) -> String {
    let st = ctx.shared.eng.lock().unwrap().status(Instant::now());
    let mode = if st.mode == Mode::Delayed { "delayed" } else { "live" };
    let mut j = format!(
        "{{\"mode\":\"{mode}\",\"input\":{},\"delay\":{:.1},\"buffer_s\":{:.1},\"buffer_mb\":{:.1},\"arm_delay\":{},\"armed\":{},\"cut_pending\":{},\"in_kbps\":{:.0},\"overlay_rev\":{},\"default_delay\":{},\"dests\":[",
        st.input,
        st.delay_s,
        st.buffer_s,
        st.buffer_mb,
        st.arm_delay_s.map(|d| format!("{d:.1}")).unwrap_or("null".into()),
        st.armed,
        st.cut_pending,
        st.in_kbps,
        ctx.overlay.rev(),
        ctx.default_delay.as_secs_f64(),
    );
    for (i, dest) in ctx.dests.iter().enumerate() {
        let d = dest.status.lock().unwrap();
        let _ = write!(
            j,
            "{}{{\"name\":\"{}\",\"state\":\"{}\",\"kbps\":{:.0},\"on\":{},\"autostart\":{},\"error\":\"{}\"}}",
            if i > 0 { "," } else { "" },
            esc(&d.name),
            d.state,
            d.kbps,
            dest.is_on(),
            dest.autostart,
            esc(&d.error)
        );
    }
    j.push_str("]}");
    j
}

/// Runs an action; shared by the HTTP API, the tray menu and hotkeys.
pub fn act(ctx: &Ctx, action: &str, delay: Option<f64>) -> Result<(), String> {
    let now = Instant::now();
    let r = ctx.shared.with(|e| match action {
        "arm" => {
            let d = delay.map(Duration::from_secs_f64).unwrap_or(ctx.default_delay);
            if d.is_zero() || d > Duration::from_secs(3600) {
                return Err("delay_range".to_string());
            }
            e.arm(d);
            Ok(())
        }
        "activate" => e.activate(now).map_err(str::to_string),
        "cut" => {
            e.cut(now);
            Ok(())
        }
        "cut-after" => {
            e.cut_after_airs();
            Ok(())
        }
        "disarm" => {
            e.disarm();
            Ok(())
        }
        _ => Err("unknown_action".to_string()),
    });
    match &r {
        Ok(()) => log!("action: {action}"),
        Err(e) => log!("action {action} refused: {e}"),
    }
    r
}

pub async fn run(addr: String, ctx: Arc<Ctx>) -> io::Result<()> {
    let listener = TcpListener::bind(&addr).await?;
    log!("API: http://{addr}/state");
    loop {
        let (sock, _) = listener.accept().await?;
        let ctx = ctx.clone();
        tokio::spawn(async move {
            let _ = handle(sock, &ctx).await;
        });
    }
}

async fn respond_typed(sock: &mut TcpStream, code: &str, ctype: &str, body: &str) -> io::Result<()> {
    let r = format!(
        "HTTP/1.1 {code}\r\nContent-Type: {ctype}; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    sock.write_all(r.as_bytes()).await
}

async fn respond(sock: &mut TcpStream, code: &str, body: &str) -> io::Result<()> {
    respond_typed(sock, code, "application/json", body).await
}

async fn handle(mut sock: TcpStream, ctx: &Ctx) -> io::Result<()> {
    let mut buf = Vec::with_capacity(1024);
    let mut tmp = [0u8; 1024];
    while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
        let n = tokio::time::timeout(Duration::from_secs(5), sock.read(&mut tmp)).await.map_err(|_| io::Error::other("timeout"))??;
        if n == 0 || buf.len() > 16 * 1024 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
    }
    let header_end = buf.windows(4).position(|w| w == b"\r\n\r\n").unwrap_or(0) + 4;
    let head = String::from_utf8_lossy(&buf[..header_end]).to_string();
    let content_length = head
        .lines()
        .find_map(|l| l.to_ascii_lowercase().strip_prefix("content-length:").map(|v| v.trim().parse::<usize>().unwrap_or(0)))
        .unwrap_or(0);
    if content_length > 64 * 1024 {
        return respond(&mut sock, "413 Payload Too Large", "{\"error\":\"too_large\"}").await;
    }
    let mut body = buf[header_end..].to_vec();
    while body.len() < content_length {
        let n = tokio::time::timeout(Duration::from_secs(5), sock.read(&mut tmp)).await.map_err(|_| io::Error::other("timeout"))??;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }
    body.truncate(content_length);
    let mut lines = head.lines();
    let mut parts = lines.next().unwrap_or("").split_whitespace();
    let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or("/"));
    // A web page in a browser must not be able to cut your stream: browsers mark
    // cross-site requests with Sec-Fetch-Site, while Stream Deck, curl and the plugin don't.
    let cross_site = lines.any(|l| {
        let l = l.to_ascii_lowercase();
        l.starts_with("sec-fetch-site:") && !(l.contains("none") || l.contains("same-origin"))
    });
    if cross_site {
        return respond(&mut sock, "403 Forbidden", "{\"error\":\"cross-site\"}").await;
    }
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let delay = query.split('&').find_map(|kv| kv.strip_prefix("delay=")).and_then(|v| v.parse::<f64>().ok());
    match (method, path) {
        ("GET", "/state") => respond(&mut sock, "200 OK", &state_json(ctx)).await,
        ("GET", "/events") => events(sock, ctx).await,
        ("GET", "/overlay") => respond_typed(&mut sock, "200 OK", "text/html", OVERLAY_HTML).await,
        ("GET", "/edit-theme") => respond_typed(&mut sock, "200 OK", "text/html", EDITOR_HTML).await,
        ("GET", "/overlay/config") => respond(&mut sock, "200 OK", &ctx.overlay.get()).await,
        ("POST", "/overlay/config") => match ctx.overlay.set(&String::from_utf8_lossy(&body)) {
            Ok(()) => {
                ctx.shared.state.send_modify(|v| *v += 1);
                respond(&mut sock, "200 OK", "{\"ok\":true}").await
            }
            Err(e) => respond(&mut sock, "400 Bad Request", &format!("{{\"error\":\"{e}\"}}")).await,
        },
        ("GET", "/live-preview") => respond_typed(&mut sock, "200 OK", "text/html", PREVIEW_HTML).await,
        ("GET", "/live-preview/out.flv") => crate::preview::serve(sock, &ctx.shared, crate::preview::Feed::Output).await,
        ("GET", "/live-preview/in.flv") => crate::preview::serve(sock, &ctx.shared, crate::preview::Feed::Input).await,
        ("GET" | "POST", "/dest/start" | "/dest/stop") => {
            let i = query.split('&').find_map(|kv| kv.strip_prefix("i=")).and_then(|v| v.parse::<usize>().ok());
            match i.and_then(|i| ctx.dests.get(i)) {
                Some(d) => {
                    let on = path == "/dest/start";
                    d.set_on(on);
                    log!("{}: {}", d.status.lock().unwrap().name, if on { "start" } else { "stop" });
                    ctx.shared.state.send_modify(|v| *v += 1);
                    respond(&mut sock, "200 OK", &state_json(ctx)).await
                }
                None => respond(&mut sock, "404 Not Found", "{\"error\":\"unknown_destination\"}").await,
            }
        }
        ("GET" | "POST", "/arm" | "/activate" | "/cut" | "/cut-after" | "/disarm") => match act(ctx, &path[1..], delay) {
            Ok(()) => respond(&mut sock, "200 OK", &state_json(ctx)).await,
            Err(e) => respond(&mut sock, "409 Conflict", &format!("{{\"error\":\"{}\"}}", esc(&e))).await,
        },
        _ => respond(&mut sock, "404 Not Found", "{\"error\":\"not found\"}").await,
    }
}

/// Server-sent events: one line of JSON per change, plus a refresh every
/// second while the buffer is filling so the progress moves.
async fn events(mut sock: TcpStream, ctx: &Ctx) -> io::Result<()> {
    sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n").await?;
    let mut rx = ctx.shared.state.subscribe();
    loop {
        let j = state_json(ctx);
        sock.write_all(format!("data: {j}\n\n").as_bytes()).await?;
        // Once a second while anything moves (buffer filling, a stream in or out), so
        // progress and bitrate charts stay live; otherwise only on changes.
        let filling = j.contains("\"armed\":false") && !j.contains("\"arm_delay\":null");
        let busy = filling || j.contains("\"input\":true") || j.contains("\"state\":\"live\"");
        let tick = if busy { Duration::from_secs(1) } else { Duration::from_secs(15) };
        tokio::select! {
            r = rx.changed() => if r.is_err() { return Ok(()) },
            _ = tokio::time::sleep(tick) => {}
        }
        // Coalesce bursts of changes into one update.
        tokio::time::sleep(Duration::from_millis(50)).await;
        rx.borrow_and_update();
    }
}
