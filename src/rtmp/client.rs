//! Egress: publishes the delayed stream to one destination (Twitch, YouTube, Kick, ...).

use super::amf0::{obj, s, Amf};
use super::chunk::*;
use super::*;
use crate::engine::{OutMsg, OutTag, Shared};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::io::{split, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio::sync::{broadcast, mpsc, watch};

pub trait Io: AsyncRead + AsyncWrite + Unpin + Send {}
impl<T: AsyncRead + AsyncWrite + Unpin + Send> Io for T {}

#[derive(Clone, Debug)]
pub struct Target {
    pub tls: bool,
    pub host: String,
    pub port: u16,
    pub app: String,
    pub tc_url: String,
    pub key: String,
}

/// Parses `rtmp[s]://host[:port]/app[/...]`; the stream key is passed separately.
pub fn parse_url(url: &str, key: &str) -> Result<Target, String> {
    let (tls, rest) = if let Some(r) = url.strip_prefix("rtmps://") {
        (true, r)
    } else if let Some(r) = url.strip_prefix("rtmp://") {
        (false, r)
    } else {
        return Err(format!("the address must start with rtmp:// or rtmps://: {url}"));
    };
    let (hostport, app) = rest.split_once('/').unwrap_or((rest, ""));
    let (host, port) = match hostport.rsplit_once(':') {
        Some((h, p)) => (h, p.parse().map_err(|_| format!("bad port in {url}"))?),
        None => (hostport, if tls { 443 } else { 1935 }),
    };
    if host.is_empty() {
        return Err(format!("no host in {url}"));
    }
    Ok(Target {
        tls,
        host: host.to_string(),
        port,
        app: app.trim_end_matches('/').to_string(),
        tc_url: url.trim_end_matches('/').to_string(),
        key: key.to_string(),
    })
}

#[derive(Clone, Debug, Default)]
pub struct DestStatus {
    pub name: String,
    /// stopped, waiting, connecting, live or error.
    pub state: &'static str,
    pub error: String,
    pub kbps: f64,
}

/// One destination as the rest of the app sees it: its status, an on/off switch, and the ingest it dials.
pub struct Dest {
    pub status: Mutex<DestStatus>,
    pub autostart: bool,
    want: watch::Sender<bool>,
    target: watch::Sender<Target>,
}

impl Dest {
    pub fn new(name: &str, autostart: bool, target: Target) -> Arc<Self> {
        let (want, _) = watch::channel(autostart);
        let (target_tx, _) = watch::channel(target);
        Arc::new(Self {
            status: Mutex::new(DestStatus { name: name.to_string(), state: "waiting", ..Default::default() }),
            autostart,
            want,
            target: target_tx,
        })
    }

    /// Point the next connection at a different ingest. An open publish drops and redials.
    /// Used for Twitch GoLive: the ladder must not go out on the classic RTMP url.
    pub fn set_target(&self, target: Target) {
        self.target.send_replace(target);
    }

    pub fn is_on(&self) -> bool {
        *self.want.borrow()
    }

    /// Start (or stop) sending to this destination. Takes effect immediately.
    pub fn set_on(&self, on: bool) {
        self.want.send_replace(on);
    }
}

async fn open(t: &Target) -> io::Result<Box<dyn Io>> {
    let tcp = tokio::time::timeout(Duration::from_secs(10), TcpStream::connect((t.host.as_str(), t.port)))
        .await
        .map_err(|_| io::Error::other("connection timed out"))??;
    tcp.set_nodelay(true)?;
    if !t.tls {
        return Ok(Box::new(tcp));
    }
    let cx = tokio_native_tls::native_tls::TlsConnector::new().map_err(io::Error::other)?;
    let tls = tokio_native_tls::TlsConnector::from(cx).connect(&t.host, tcp).await.map_err(io::Error::other)?;
    Ok(Box::new(tls))
}

struct Conn {
    io: Box<dyn Io>,
    reader: ChunkReader,
    w: ChunkWriter,
    out: Vec<u8>,
}

impl Conn {
    async fn send(&mut self, csid: u8, m: &Message) -> io::Result<()> {
        self.w.encode(csid, m, &mut self.out);
        self.io.write_all(&self.out).await?;
        self.out.clear();
        Ok(())
    }

    /// Reads until a `_result`/`onStatus` answers `txn` (or any onStatus when txn is 0).
    async fn wait_reply(&mut self, txn: f64) -> io::Result<Vec<Amf>> {
        loop {
            let m = self.reader.read(&mut self.io).await?;
            if m.typ != MSG_CMD_AMF0 {
                continue;
            }
            let v = amf0::decode_all(&m.payload);
            let name = v.first().and_then(Amf::as_str).unwrap_or("");
            let id = v.get(1).and_then(Amf::as_num).unwrap_or(-1.0);
            match name {
                "_result" if id == txn => return Ok(v),
                "_error" if id == txn => return Err(io::Error::other(describe(&v))),
                "onStatus" if txn == 0.0 => return Ok(v),
                _ => {}
            }
        }
    }
}

fn describe(v: &[Amf]) -> String {
    let info = v.iter().find(|x| x.get("code").is_some());
    let code = info.and_then(|i| i.get("code")).and_then(Amf::as_str).unwrap_or("?");
    let desc = info.and_then(|i| i.get("description")).and_then(Amf::as_str).unwrap_or("");
    format!("{code} {desc}").trim().to_string()
}

async fn publish(t: &Target) -> io::Result<(Conn, u32)> {
    let mut io = open(t).await?;
    handshake_client(&mut io).await?;
    let mut c = Conn { io, reader: ChunkReader::default(), w: ChunkWriter::default(), out: Vec::new() };
    c.send(CS_CONTROL, &control(MSG_SET_CHUNK_SIZE, OUT_CHUNK_SIZE as u32)).await?;
    c.w.chunk_size = OUT_CHUNK_SIZE;
    let connect = obj(&[
        ("app", s(&t.app)),
        ("type", s("nonprivate")),
        ("flashVer", s("FMLE/3.0 (compatible; FMSc/1.0)")),
        ("tcUrl", s(&t.tc_url)),
    ]);
    c.send(CS_COMMAND, &command(0, &[s("connect"), Amf::Number(1.0), connect])).await?;
    c.wait_reply(1.0).await?;
    c.send(CS_COMMAND, &command(0, &[s("releaseStream"), Amf::Number(2.0), Amf::Null, s(&t.key)])).await?;
    c.send(CS_COMMAND, &command(0, &[s("FCPublish"), Amf::Number(3.0), Amf::Null, s(&t.key)])).await?;
    c.send(CS_COMMAND, &command(0, &[s("createStream"), Amf::Number(4.0), Amf::Null])).await?;
    let r = c.wait_reply(4.0).await?;
    let sid = r.get(3).and_then(Amf::as_num).unwrap_or(1.0) as u32;
    c.send(CS_COMMAND, &command(sid, &[s("publish"), Amf::Number(5.0), Amf::Null, s(&t.key), s("live")])).await?;
    let st = c.wait_reply(0.0).await?;
    let d = describe(&st);
    if !d.starts_with("NetStream.Publish.Start") {
        return Err(io::Error::other(d));
    }
    Ok((c, sid))
}

fn media_message(t: &OutTag, base: u64, sid: u32) -> (u8, Message) {
    let csid = match t.kind {
        MSG_AUDIO => CS_AUDIO,
        MSG_VIDEO => CS_VIDEO,
        _ => CS_DATA,
    };
    let ts = t.ts.saturating_sub(base) as u32;
    (csid, Message { typ: t.kind, ts, stream_id: sid, payload: (*t.data).clone() })
}

enum Ended {
    StreamEnd,
    /// Switched off from LiveDelay.
    Stopped,
    Error(io::Error),
}

/// One publishing session: sends config, then waits for a keyframe and streams.
async fn stream(
    c: Conn,
    sid: u32,
    rx: &mut broadcast::Receiver<OutMsg>,
    want: &mut watch::Receiver<bool>,
    shared: &Shared,
    status: &Mutex<DestStatus>,
) -> Ended {
    let Conn { io, mut reader, w, mut out } = c;
    let (rd, mut wr) = split(io);
    // The server may ping us; answer from the writer side.
    let (ping_tx, mut ping_rx) = mpsc::channel::<Vec<u8>>(4);
    let reader_task = tokio::spawn(async move {
        let mut rd = BufReader::new(rd);
        loop {
            let m = match reader.read(&mut rd).await {
                Ok(m) => m,
                Err(e) => return e,
            };
            if m.typ == MSG_USER_CONTROL && m.payload.len() >= 6 && m.payload[..2] == [0, 6] {
                let _ = ping_tx.send(m.payload[2..6].to_vec()).await;
            }
            if m.typ == MSG_CMD_AMF0 {
                let v = amf0::decode_all(&m.payload);
                if v.first().and_then(Amf::as_str) == Some("onStatus") {
                    let d = describe(&v);
                    if d.contains("Error") || d.contains("Failed") || d.contains("Rejected") {
                        return io::Error::other(d);
                    }
                }
            }
        }
    });
    let mut base: Option<u64> = None;
    let mut need_key = true;
    let mut sent_bytes = 0u64;
    let mut window_start = Instant::now();
    let initial = shared.eng.lock().unwrap().current_config();
    let mut pending: Vec<OutTag> = initial;

    let result = loop {
        let msg = tokio::select! {
            m = rx.recv() => m,
            _ = want.changed() => {
                if *want.borrow_and_update() { continue }
                break Ended::Stopped;
            }
            p = ping_rx.recv() => {
                match p {
                    Some(p) => {
                        w.encode(CS_CONTROL, &user_control(7, &p), &mut out);
                        if let Err(e) = wr.write_all(&out).await { break Ended::Error(e); }
                        out.clear();
                        continue;
                    }
                    None => break Ended::Error(io::Error::other("server closed the connection")),
                }
            }
        };
        let tag = match msg {
            Ok(OutMsg::Tag(t)) => t,
            Ok(OutMsg::End) => break Ended::StreamEnd,
            Err(broadcast::error::RecvError::Lagged(_)) => {
                // Reseed codec headers or Twitch reconnects with a header-less stream (#1000).
                need_key = true;
                pending = shared.eng.lock().unwrap().current_config();
                continue;
            }
            Err(broadcast::error::RecvError::Closed) => break Ended::StreamEnd,
        };
        if tag.config {
            if need_key {
                // Keep distinct video headers (one per Enhanced quality) and AAC headers (live + VOD).
                if tag.kind == MSG_AUDIO || tag.kind == MSG_VIDEO {
                    if let Some(i) = pending.iter().position(|p| p.kind == tag.kind && p.data == tag.data) {
                        pending[i] = tag;
                    } else if pending.iter().filter(|p| p.kind == tag.kind).count() < 6 {
                        pending.push(tag);
                    }
                } else {
                    pending.retain(|p| p.kind != tag.kind);
                    pending.push(tag);
                }
                continue;
            }
        } else if need_key {
            if !(tag.kind == MSG_VIDEO && tag.key) {
                continue;
            }
            need_key = false;
            let b = *base.get_or_insert(tag.ts);
            for p in pending.drain(..) {
                let (cs, m) = media_message(&OutTag { ts: tag.ts, ..p }, b, sid);
                w.encode(cs, &m, &mut out);
            }
            // First keyframe out → safe to show as live (Twitch has real media).
            {
                let mut s = status.lock().unwrap();
                if s.state != "live" {
                    s.state = "live";
                    s.error.clear();
                    log!("{}: live (keyframe sent)", s.name);
                }
            }
            shared.state.send_modify(|v| *v += 1);
        }
        let b = *base.get_or_insert(tag.ts);
        let (cs, m) = media_message(&tag, b, sid);
        w.encode(cs, &m, &mut out);
        sent_bytes += out.len() as u64;
        if let Err(e) = wr.write_all(&out).await {
            break Ended::Error(e);
        }
        out.clear();
        let el = window_start.elapsed();
        if el >= Duration::from_secs(1) {
            status.lock().unwrap().kbps = sent_bytes as f64 * 8.0 / 1000.0 / el.as_secs_f64();
            sent_bytes = 0;
            window_start = Instant::now();
        }
    };
    if matches!(result, Ended::StreamEnd | Ended::Stopped) {
        let mut bye = Vec::new();
        w.encode(CS_COMMAND, &command(0, &[s("FCUnpublish"), Amf::Number(6.0), Amf::Null, s("")]), &mut bye);
        w.encode(CS_COMMAND, &command(0, &[s("deleteStream"), Amf::Number(7.0), Amf::Null, Amf::Number(sid as f64)]), &mut bye);
        let _ = wr.write_all(&bye).await;
        let _ = wr.shutdown().await;
    }
    reader_task.abort();
    result
}

enum SessionEnd {
    Clean,
    Fail { connected: bool, err: io::Error },
}

async fn session(
    target: Target,
    rx: &mut broadcast::Receiver<OutMsg>,
    want: &mut watch::Receiver<bool>,
    shared: &Shared,
    dest: &Dest,
    name: &str,
) -> SessionEnd {
    match publish(&target).await {
        Ok((conn, sid)) => {
            log!("{name}: connected (waiting for keyframe before live)");
            // Stay "connecting" until stream() has sent a keyframe — avoids
            // Twitch announcing LIVE with an empty/broken publish (#1000).
            match stream(conn, sid, rx, want, shared, &dest.status).await {
                Ended::StreamEnd => {
                    log!("{name}: stream ended");
                    dest.set_on(dest.autostart);
                    SessionEnd::Clean
                }
                Ended::Stopped => {
                    log!("{name}: stopped from LiveDelay");
                    SessionEnd::Clean
                }
                Ended::Error(e) => {
                    log!("{name}: dropped: {e}");
                    SessionEnd::Fail { connected: true, err: e }
                }
            }
        }
        Err(e) => {
            log!("{name}: could not connect: {e}");
            SessionEnd::Fail { connected: false, err: e }
        }
    }
}

/// Keeps one destination connected while it is switched on and there is something to send.
pub async fn run(dest: Arc<Dest>, shared: Arc<Shared>) {
    let name = dest.status.lock().unwrap().name.clone();
    let set = |state: &'static str, error: String| {
        let mut s = dest.status.lock().unwrap();
        s.state = state;
        s.error = error;
        if state != "live" {
            s.kbps = 0.0;
        }
        drop(s);
        shared.state.send_modify(|v| *v += 1);
    };
    let mut want = dest.want.subscribe();
    let mut target_rx = dest.target.subscribe();
    let mut rx = shared.out.subscribe();
    let mut backoff = Duration::from_secs(2);
    loop {
        if !*want.borrow_and_update() {
            set("stopped", String::new());
            if want.changed().await.is_err() {
                return;
            }
            // Tags piled up while switched off are old; start from what is current.
            rx = rx.resubscribe();
            continue;
        }
        set("waiting", String::new());
        // Wait for video (config or keyframe) before dialing Twitch — avoids an empty
        // "live" publish that the player aborts with error #1000.
        let go = loop {
            let has_video = shared.eng.lock().unwrap().current_config().iter().any(|t| t.kind == MSG_VIDEO);
            if has_video {
                break true;
            }
            tokio::select! {
                r = rx.recv() => match r {
                    Ok(OutMsg::Tag(t)) => {
                        if t.kind == MSG_VIDEO {
                            break true;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {
                        if shared.eng.lock().unwrap().current_config().iter().any(|t| t.kind == MSG_VIDEO) {
                            break true;
                        }
                    }
                    Ok(OutMsg::End) => continue,
                    Err(broadcast::error::RecvError::Closed) => return,
                },
                _ = want.changed() => if !*want.borrow_and_update() { break false },
                _ = target_rx.changed() => break false,
            }
        };
        if !go {
            continue;
        }
        let target = target_rx.borrow_and_update().clone();
        set("connecting", String::new());
        tokio::select! {
            _ = target_rx.changed() => {
                log!("{name}: ingest changed, reconnecting");
                rx = rx.resubscribe();
            }
            end = session(target, &mut rx, &mut want, &shared, &dest, &name) => match end {
                SessionEnd::Clean => {}
                SessionEnd::Fail { connected, err } => {
                    if connected {
                        backoff = Duration::from_secs(2);
                    }
                    set("error", err.to_string());
                    tokio::select! {
                        _ = tokio::time::sleep(backoff) => {}
                        _ = want.changed() => {}
                        _ = target_rx.changed() => {}
                    }
                    backoff = (backoff * 2).min(Duration::from_secs(30));
                    rx = rx.resubscribe();
                }
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_urls() {
        let t = parse_url("rtmp://waw.contribute.live-video.net/app/", "k").unwrap();
        assert_eq!((t.tls, t.host.as_str(), t.port, t.app.as_str()), (false, "waw.contribute.live-video.net", 1935, "app"));
        let t = parse_url("rtmps://fa723fc1b171.global-contribute.live-video.net:443/app", "k").unwrap();
        assert_eq!((t.tls, t.port), (true, 443));
        let t = parse_url("rtmp://127.0.0.1:1940/live", "x").unwrap();
        assert_eq!((t.port, t.tc_url.as_str()), (1940, "rtmp://127.0.0.1:1940/live"));
        assert!(parse_url("http://x", "k").is_err());
    }
}
