//! Ingest: accepts RTMP publishes from OBS (one key per platform, plus `_idle`).

use super::amf0::{obj, s, Amf};
use super::chunk::*;
use super::*;
use crate::engine::Shared;
use crate::hub::Hub;
use std::sync::Arc;
use std::time::Instant;
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};

const WINDOW: u32 = 2_500_000;
const STREAM_ID: u32 = 1;

pub async fn run(addr: String, hub: Arc<Hub>) -> io::Result<()> {
    let listener = TcpListener::bind(&addr).await?;
    log!("RTMP: waiting for OBS on rtmp://{addr}/live (keys ld0… / _idle)");
    loop {
        let (sock, peer) = listener.accept().await?;
        let _ = sock.set_nodelay(true);
        let hub = hub.clone();
        tokio::spawn(async move {
            match handle(sock, &hub).await {
                Ok(()) => log!("RTMP: {peer} disconnected"),
                Err(e) => log!("RTMP: {peer} disconnected: {e}"),
            }
        });
    }
}

struct Session {
    out: Vec<u8>,
    w: ChunkWriter,
}

impl Session {
    fn send(&mut self, csid: u8, m: Message) {
        self.w.encode(csid, &m, &mut self.out);
    }
}

fn status(code: &str, desc: &str) -> Amf {
    obj(&[("level", s("status")), ("code", s(code)), ("description", s(desc))])
}

/// Releases the publisher slot even if the connection dies mid-stream.
struct PublishGuard {
    hub: Arc<Hub>,
    key: String,
    shared: Option<Arc<Shared>>,
    /// Sequence headers seen while this publish was still discarded.
    held: Vec<(u8, u32, Vec<u8>)>,
}

fn is_seq_header(kind: u8, d: &[u8]) -> bool {
    let b0 = d.first().copied().unwrap_or(0);
    match kind {
        MSG_VIDEO if b0 & 0x80 != 0 => {
            let mut pkt = b0 & 0x0f;
            if pkt == 6 {
                pkt = d.get(1).copied().unwrap_or(0xff) & 0x0f;
            }
            pkt == 0
        }
        MSG_VIDEO => matches!(b0 & 0x0f, 7 | 12) && d.get(1) == Some(&0),
        MSG_AUDIO => {
            let fmt = b0 >> 4;
            (fmt == 10 && d.get(1) == Some(&0))
                || (fmt == 9 && (b0 & 0x0f == 0 || (b0 & 0x0f == 5 && d.get(1).copied().unwrap_or(0xff) & 0x0f == 0)))
        }
        MSG_DATA_AMF0 => true,
        _ => false,
    }
}

fn remember_header(held: &mut Vec<(u8, u32, Vec<u8>)>, kind: u8, ts: u32, payload: &[u8]) {
    if !is_seq_header(kind, payload) {
        return;
    }
    if let Some(slot) = held.iter_mut().find(|h| h.0 == kind) {
        *slot = (kind, ts, payload.to_vec());
    } else {
        held.push((kind, ts, payload.to_vec()));
    }
}

/// A stream already connected as `_idle` starts feeding the preview on the
/// next frame after the option is turned on. Headers seen before that are
/// replayed so the picture can decode without restarting OBS.
fn adopt_idle(g: &mut PublishGuard) {
    if g.shared.is_some() {
        return;
    }
    let Some(shared) = g.hub.adopt_idle(&g.key) else { return };
    let held = std::mem::take(&mut g.held);
    shared.with(|e| {
        e.begin_session();
        let now = Instant::now();
        for (kind, ts, payload) in held {
            e.push(kind, ts, payload, now);
        }
    });
    log!("RTMP: OBS publish ({}) now feeds the preview", g.key);
    g.shared = Some(shared);
}

impl Drop for PublishGuard {
    fn drop(&mut self) {
        if let Some(shared) = &self.shared {
            shared.with(|e| e.end_session());
            log!("RTMP: OBS stopped streaming ({})", self.key);
        } else {
            log!("RTMP: OBS idle publish ended");
        }
        self.hub.release(&self.key);
    }
}

fn publish_key(v: &[Amf]) -> String {
    // publish(null, streamName, type)
    v.get(3).and_then(Amf::as_str).unwrap_or("").to_string()
}

async fn handle(mut sock: TcpStream, hub: &Arc<Hub>) -> io::Result<()> {
    handshake_server(&mut sock).await?;
    let (rd, mut wr) = sock.into_split();
    let mut rd = BufReader::with_capacity(64 * 1024, rd);
    let mut reader = ChunkReader::default();
    let mut ses = Session { out: Vec::new(), w: ChunkWriter::default() };
    let mut guard: Option<PublishGuard> = None;
    let mut peer_window = WINDOW as u64;
    let mut acked = 0u64;

    loop {
        let m = reader.read(&mut rd).await?;
        match m.typ {
            MSG_AUDIO | MSG_VIDEO => {
                if let Some(g) = guard.as_mut() {
                    if g.shared.is_none() {
                        adopt_idle(g);
                        if g.shared.is_none() {
                            remember_header(&mut g.held, m.typ, m.ts, &m.payload);
                        }
                    }
                    if let Some(shared) = &g.shared {
                        shared.with(|e| e.push(m.typ, m.ts, m.payload, Instant::now()));
                    }
                }
            }
            MSG_DATA_AMF0 => {
                if let Some(g) = guard.as_mut() {
                    if g.shared.is_none() {
                        adopt_idle(g);
                        if g.shared.is_none() {
                            remember_header(&mut g.held, m.typ, m.ts, &m.payload);
                        }
                    }
                    if let Some(shared) = &g.shared {
                        let v = amf0::decode_all(&m.payload);
                        if v.iter().any(|x| x.as_str() == Some("onMetaData")) {
                            // The publisher states the picture size here, and
                            // nowhere else we can read without decoding video.
                            if let Some(meta) = v.iter().find(|x| x.get("width").is_some()) {
                                let num = |k: &str| meta.get(k).and_then(Amf::as_num).unwrap_or(0.0);
                                shared.set_size(num("width") as u32, num("height") as u32);
                            }
                            let payload = if v.first().and_then(Amf::as_str) == Some("@setDataFrame") {
                                m.payload
                            } else {
                                let mut p = amf0::encode_all(&[s("@setDataFrame")]);
                                p.extend_from_slice(&m.payload);
                                p
                            };
                            shared.with(|e| e.push(MSG_DATA_AMF0, m.ts, payload, Instant::now()));
                        }
                    }
                }
            }
            MSG_WINDOW_ACK if m.payload.len() >= 4 => {
                peer_window = u32::from_be_bytes(m.payload[..4].try_into().unwrap()).max(1) as u64;
            }
            MSG_CMD_AMF0 => {
                let v = amf0::decode_all(&m.payload);
                let name = v.first().and_then(Amf::as_str).unwrap_or("");
                let txn = v.get(1).and_then(Amf::as_num).unwrap_or(0.0);
                match name {
                    "connect" => {
                        ses.send(CS_CONTROL, control(MSG_WINDOW_ACK, WINDOW));
                        ses.send(CS_CONTROL, peer_bandwidth(WINDOW));
                        ses.send(CS_CONTROL, control(MSG_SET_CHUNK_SIZE, OUT_CHUNK_SIZE as u32));
                        ses.w.chunk_size = OUT_CHUNK_SIZE;
                        let props = obj(&[("fmsVer", s("FMS/3,0,1,123")), ("capabilities", Amf::Number(31.0))]);
                        let mut info = status("NetConnection.Connect.Success", "Connection succeeded.");
                        if let Amf::Object(p) = &mut info {
                            p.push(("objectEncoding".into(), Amf::Number(0.0)));
                        }
                        ses.send(CS_COMMAND, command(0, &[s("_result"), Amf::Number(txn), props, info]));
                    }
                    "createStream" => {
                        ses.send(CS_COMMAND, command(0, &[s("_result"), Amf::Number(txn), Amf::Null, Amf::Number(STREAM_ID as f64)]));
                    }
                    "releaseStream" | "FCPublish" => {
                        ses.send(CS_COMMAND, command(0, &[s("_result"), Amf::Number(txn), Amf::Null, Amf::Undefined]));
                    }
                    "publish" => {
                        if guard.is_some() {
                            ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.BadName", "Already publishing")]));
                            wr.write_all(&ses.out).await?;
                            return Err(io::Error::other("second publish on same connection"));
                        }
                        let key = publish_key(&v);
                        match hub.try_claim(&key) {
                            Ok(shared) => {
                                if let Some(ref s) = shared {
                                    s.with(|e| e.begin_session());
                                    log!("RTMP: OBS is streaming as {key}");
                                } else {
                                    log!("RTMP: OBS idle/discard publish ({key})");
                                }
                                guard = Some(PublishGuard { hub: hub.clone(), key, shared, held: Vec::new() });
                                ses.send(CS_CONTROL, user_control(0, &STREAM_ID.to_be_bytes()));
                                ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.Start", "Publishing")]));
                            }
                            Err("busy") => {
                                ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.BadName", "Already publishing")]));
                                wr.write_all(&ses.out).await?;
                                return Err(io::Error::other("publisher busy"));
                            }
                            Err(_) => {
                                ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.BadName", "Unknown stream key")]));
                                wr.write_all(&ses.out).await?;
                                return Err(io::Error::other(format!("unknown stream key")));
                            }
                        }
                    }
                    "FCUnpublish" | "deleteStream" | "closeStream" => {
                        guard = None;
                    }
                    _ => {}
                }
            }
            _ => {}
        }
        if reader.bytes_read - acked >= peer_window / 2 {
            acked = reader.bytes_read;
            ses.send(CS_CONTROL, control(MSG_ACK, acked as u32));
        }
        if !ses.out.is_empty() {
            wr.write_all(&ses.out).await?;
            ses.out.clear();
        }
    }
}
