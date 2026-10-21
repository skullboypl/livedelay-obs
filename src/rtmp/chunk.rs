//! RTMP chunk stream: splitting messages into chunks and putting them back together.

use std::collections::HashMap;
use std::io;
use tokio::io::{AsyncRead, AsyncReadExt};

pub const MSG_SET_CHUNK_SIZE: u8 = 1;
pub const MSG_ABORT: u8 = 2;
pub const MSG_ACK: u8 = 3;
pub const MSG_USER_CONTROL: u8 = 4;
pub const MSG_WINDOW_ACK: u8 = 5;
pub const MSG_PEER_BW: u8 = 6;
pub const MSG_AUDIO: u8 = 8;
pub const MSG_VIDEO: u8 = 9;
pub const MSG_DATA_AMF0: u8 = 18;
pub const MSG_CMD_AMF0: u8 = 20;

const MAX_MESSAGE: usize = 32 * 1024 * 1024;

#[derive(Debug, Clone)]
pub struct Message {
    pub typ: u8,
    pub ts: u32,
    pub stream_id: u32,
    pub payload: Vec<u8>,
}

#[derive(Default)]
struct ChunkStream {
    ts: u32,
    /// The timestamp field of the last header: absolute after type 0, a delta after 1 and 2.
    ts_field: u32,
    extended: bool,
    len: usize,
    typ: u8,
    stream_id: u32,
    buf: Vec<u8>,
}

pub struct ChunkReader {
    chunk_size: usize,
    streams: HashMap<u32, ChunkStream>,
    pub bytes_read: u64,
}

impl Default for ChunkReader {
    fn default() -> Self {
        Self { chunk_size: 128, streams: HashMap::new(), bytes_read: 0 }
    }
}

async fn u24<R: AsyncRead + Unpin>(r: &mut R) -> io::Result<u32> {
    let mut b = [0u8; 3];
    r.read_exact(&mut b).await?;
    Ok(u32::from_be_bytes([0, b[0], b[1], b[2]]))
}

fn bad(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_string())
}

impl ChunkReader {
    /// Reads chunks until one whole message is assembled.
    pub async fn read<R: AsyncRead + Unpin>(&mut self, r: &mut R) -> io::Result<Message> {
        loop {
            let b0 = r.read_u8().await?;
            let fmt = b0 >> 6;
            let mut header_len = 1u64;
            let csid = match b0 & 0x3f {
                0 => {
                    header_len += 1;
                    64 + r.read_u8().await? as u32
                }
                1 => {
                    header_len += 2;
                    let lo = r.read_u8().await? as u32;
                    let hi = r.read_u8().await? as u32;
                    64 + lo + hi * 256
                }
                n => n as u32,
            };
            let cs = self.streams.entry(csid).or_default();
            let starting = cs.buf.is_empty();
            match fmt {
                0 => {
                    let ts = u24(r).await?;
                    cs.len = u24(r).await? as usize;
                    cs.typ = r.read_u8().await?;
                    cs.stream_id = r.read_u32_le().await?;
                    header_len += 11;
                    cs.extended = ts == 0xFF_FFFF;
                    let ts = if cs.extended {
                        header_len += 4;
                        r.read_u32().await?
                    } else {
                        ts
                    };
                    cs.ts = ts;
                    cs.ts_field = ts;
                }
                1 | 2 => {
                    let delta = u24(r).await?;
                    header_len += 3;
                    if fmt == 1 {
                        cs.len = u24(r).await? as usize;
                        cs.typ = r.read_u8().await?;
                        header_len += 4;
                    }
                    cs.extended = delta == 0xFF_FFFF;
                    let delta = if cs.extended {
                        header_len += 4;
                        r.read_u32().await?
                    } else {
                        delta
                    };
                    cs.ts_field = delta;
                    cs.ts = cs.ts.wrapping_add(delta);
                }
                _ => {
                    if cs.extended {
                        header_len += 4;
                        r.read_u32().await?;
                    }
                    if starting {
                        cs.ts = cs.ts.wrapping_add(cs.ts_field);
                    }
                }
            }
            if cs.len > MAX_MESSAGE {
                return Err(bad("message too large"));
            }
            let want = (cs.len - cs.buf.len()).min(self.chunk_size);
            let start = cs.buf.len();
            cs.buf.resize(start + want, 0);
            r.read_exact(&mut cs.buf[start..]).await?;
            self.bytes_read += header_len + want as u64;
            if cs.buf.len() < cs.len {
                continue;
            }
            let msg = Message {
                typ: cs.typ,
                ts: cs.ts,
                stream_id: cs.stream_id,
                payload: std::mem::take(&mut cs.buf),
            };
            match msg.typ {
                MSG_SET_CHUNK_SIZE if msg.payload.len() >= 4 => {
                    let n = u32::from_be_bytes(msg.payload[..4].try_into().unwrap()) & 0x7fff_ffff;
                    self.chunk_size = (n as usize).clamp(1, MAX_MESSAGE);
                }
                MSG_ABORT if msg.payload.len() >= 4 => {
                    let id = u32::from_be_bytes(msg.payload[..4].try_into().unwrap());
                    if let Some(s) = self.streams.get_mut(&id) {
                        s.buf.clear();
                    }
                }
                _ => {}
            }
            return Ok(msg);
        }
    }
}

/// Serializes messages into chunks. Every message gets a full (type 0) header,
/// which costs a few bytes but keeps the writer stateless and always valid.
pub struct ChunkWriter {
    pub chunk_size: usize,
}

impl Default for ChunkWriter {
    fn default() -> Self {
        Self { chunk_size: 128 }
    }
}

impl ChunkWriter {
    pub fn encode(&self, csid: u8, m: &Message, out: &mut Vec<u8>) {
        debug_assert!((2..64).contains(&csid));
        let ext = m.ts >= 0xFF_FFFF;
        let ts_field = if ext { 0xFF_FFFF } else { m.ts };
        out.push(csid);
        out.extend_from_slice(&ts_field.to_be_bytes()[1..]);
        out.extend_from_slice(&(m.payload.len() as u32).to_be_bytes()[1..]);
        out.push(m.typ);
        out.extend_from_slice(&m.stream_id.to_le_bytes());
        if ext {
            out.extend_from_slice(&m.ts.to_be_bytes());
        }
        let mut chunks = m.payload.chunks(self.chunk_size.max(1));
        if let Some(first) = chunks.next() {
            out.extend_from_slice(first);
        }
        for c in chunks {
            out.push(0xC0 | csid);
            if ext {
                out.extend_from_slice(&m.ts.to_be_bytes());
            }
            out.extend_from_slice(c);
        }
    }
}

pub fn control(typ: u8, value: u32) -> Message {
    Message { typ, ts: 0, stream_id: 0, payload: value.to_be_bytes().to_vec() }
}

pub fn peer_bandwidth(value: u32) -> Message {
    let mut payload = value.to_be_bytes().to_vec();
    payload.push(2); // dynamic
    Message { typ: MSG_PEER_BW, ts: 0, stream_id: 0, payload }
}

pub fn user_control(event: u16, data: &[u8]) -> Message {
    let mut payload = event.to_be_bytes().to_vec();
    payload.extend_from_slice(data);
    Message { typ: MSG_USER_CONTROL, ts: 0, stream_id: 0, payload }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test(flavor = "current_thread")]
    async fn roundtrip_with_small_and_extended_chunks() {
        let w = ChunkWriter { chunk_size: 100 };
        let msgs = [
            Message { typ: MSG_VIDEO, ts: 40, stream_id: 1, payload: (0..=255u8).cycle().take(1000).collect() },
            Message { typ: MSG_AUDIO, ts: 0x0100_0000, stream_id: 1, payload: vec![7; 250] },
        ];
        let mut wire = Vec::new();
        // Tell the reader about the chunk size the same way a peer would.
        w.encode(2, &control(MSG_SET_CHUNK_SIZE, 100), &mut wire);
        for m in &msgs {
            w.encode(6, m, &mut wire);
        }
        let mut r = ChunkReader::default();
        let mut src = &wire[..];
        assert_eq!(r.read(&mut src).await.unwrap().typ, MSG_SET_CHUNK_SIZE);
        for m in &msgs {
            let got = r.read(&mut src).await.unwrap();
            assert_eq!((got.typ, got.ts, got.stream_id), (m.typ, m.ts, m.stream_id));
            assert_eq!(got.payload, m.payload);
        }
    }
}
