//! Ingest: accepts the RTMP publish from OBS and feeds the delay engine.

use super::amf0::{obj, s, Amf};
use super::chunk::*;
use super::*;
use crate::engine::Shared;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;
use tokio::io::{AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};

const WINDOW: u32 = 2_500_000;
const STREAM_ID: u32 = 1;

pub async fn run(addr: String, shared: Arc<Shared>) -> io::Result<()> {
    let listener = TcpListener::bind(&addr).await?;
    log!("RTMP: waiting for OBS on rtmp://{addr}/live");
    let busy = Arc::new(AtomicBool::new(false));
    loop {
        let (sock, peer) = listener.accept().await?;
        let _ = sock.set_nodelay(true);
        let shared = shared.clone();
        let busy = busy.clone();
        tokio::spawn(async move {
            match handle(sock, &shared, &busy).await {
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

/// Releases the single publisher slot even if the connection dies mid-stream.
struct PublishGuard<'a> {
    busy: &'a AtomicBool,
    shared: &'a Shared,
}

impl Drop for PublishGuard<'_> {
    fn drop(&mut self) {
        self.shared.with(|e| e.end_session());
        self.busy.store(false, Ordering::SeqCst);
        log!("RTMP: OBS stopped streaming");
    }
}

async fn handle(mut sock: TcpStream, shared: &Shared, busy: &AtomicBool) -> io::Result<()> {
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
            MSG_AUDIO | MSG_VIDEO if guard.is_some() => {
                shared.with(|e| e.push(m.typ, m.ts, m.payload, Instant::now()));
            }
            MSG_DATA_AMF0 if guard.is_some() => {
                // Forward "@setDataFrame onMetaData {...}" as the client sent it.
                let v = amf0::decode_all(&m.payload);
                if v.iter().any(|x| x.as_str() == Some("onMetaData")) {
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
                        if guard.is_some() || busy.swap(true, Ordering::SeqCst) {
                            ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.BadName", "Already publishing")]));
                            wr.write_all(&ses.out).await?;
                            return Err(io::Error::other("second publisher refused"));
                        }
                        shared.with(|e| e.begin_session());
                        guard = Some(PublishGuard { busy, shared });
                        log!("RTMP: OBS is streaming");
                        ses.send(CS_CONTROL, user_control(0, &STREAM_ID.to_be_bytes()));
                        ses.send(CS_COMMAND, command(m.stream_id, &[s("onStatus"), Amf::Number(0.0), Amf::Null, status("NetStream.Publish.Start", "Publishing")]));
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
