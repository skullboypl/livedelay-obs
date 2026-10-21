//! HTTP-FLV feeds for /live-preview: the input as OBS sends it, and the output as viewers get it.

use crate::engine::{OutMsg, OutTag, Shared};
use std::io;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::sync::broadcast;

pub enum Feed {
    Input,
    Output,
}

const MSG_VIDEO: u8 = 9;
const MSG_DATA: u8 = 18;
/// AMF0 string "@setDataFrame": RTMP-only wrapper that FLV files don't carry.
const SET_DATA_FRAME: &[u8] = b"\x02\x00\x0d@setDataFrame";

fn put_tag(out: &mut Vec<u8>, kind: u8, ts: u32, data: &[u8]) {
    let data = if kind == MSG_DATA { data.strip_prefix(SET_DATA_FRAME).unwrap_or(data) } else { data };
    let len = data.len() as u32;
    out.push(kind);
    out.extend_from_slice(&len.to_be_bytes()[1..]);
    out.extend_from_slice(&ts.to_be_bytes()[1..]);
    out.push((ts >> 24) as u8);
    out.extend_from_slice(&[0, 0, 0]);
    out.extend_from_slice(data);
    out.extend_from_slice(&(len + 11).to_be_bytes());
}

pub async fn serve(mut sock: TcpStream, shared: &Shared, feed: Feed) -> io::Result<()> {
    let (mut rx, config): (broadcast::Receiver<OutMsg>, Vec<OutTag>) = {
        let e = shared.eng.lock().unwrap();
        match feed {
            Feed::Input => (shared.input.subscribe(), e.input_config()),
            Feed::Output => (shared.out.subscribe(), e.current_config()),
        }
    };
    sock.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n").await?;
    let mut out = b"FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00".to_vec();
    let mut pending = config;
    let mut base: Option<u64> = None;
    loop {
        let tag = match rx.recv().await {
            Ok(OutMsg::Tag(t)) => t,
            Ok(OutMsg::End) => continue,
            Err(broadcast::error::RecvError::Lagged(_)) => {
                // Too slow to keep up (e.g. the tab was in the background): wait for a keyframe.
                base = None;
                pending = {
                    let e = shared.eng.lock().unwrap();
                    match feed {
                        Feed::Input => e.input_config(),
                        Feed::Output => e.current_config(),
                    }
                };
                continue;
            }
            Err(broadcast::error::RecvError::Closed) => return Ok(()),
        };
        if base.is_none() {
            if tag.config {
                pending.retain(|p| p.kind != tag.kind);
                pending.push(tag);
                continue;
            }
            if !(tag.kind == MSG_VIDEO && tag.key) {
                continue;
            }
            base = Some(tag.ts);
            for p in pending.drain(..) {
                put_tag(&mut out, p.kind, 0, &p.data);
            }
        }
        let ts = tag.ts.saturating_sub(base.unwrap_or(0)) as u32;
        put_tag(&mut out, tag.kind, ts, &tag.data);
        sock.write_all(&out).await?;
        out.clear();
    }
}
