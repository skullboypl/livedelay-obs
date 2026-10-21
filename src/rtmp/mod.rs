pub mod amf0;
pub mod chunk;
pub mod client;
pub mod server;

use std::io;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

const HANDSHAKE_LEN: usize = 1536;

/// Chunk stream ids used for everything we send.
pub const CS_CONTROL: u8 = 2;
pub const CS_COMMAND: u8 = 3;
pub const CS_AUDIO: u8 = 4;
pub const CS_DATA: u8 = 5;
pub const CS_VIDEO: u8 = 6;

/// Our outgoing chunk size. Bigger chunks mean fewer headers per video frame.
pub const OUT_CHUNK_SIZE: usize = 4096;

fn random_block() -> Vec<u8> {
    let mut x = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1) | 1;
    let mut b = vec![0u8; HANDSHAKE_LEN];
    for byte in b[8..].iter_mut() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *byte = x as u8;
    }
    b
}

/// Plain (non-digest) handshake as the accepting side. OBS's librtmp accepts it.
pub async fn handshake_server<S: AsyncRead + AsyncWrite + Unpin>(s: &mut S) -> io::Result<()> {
    let mut c0c1 = vec![0u8; 1 + HANDSHAKE_LEN];
    s.read_exact(&mut c0c1).await?;
    let mut reply = Vec::with_capacity(1 + 2 * HANDSHAKE_LEN);
    reply.push(3);
    reply.extend_from_slice(&random_block());
    reply.extend_from_slice(&c0c1[1..]);
    s.write_all(&reply).await?;
    let mut c2 = vec![0u8; HANDSHAKE_LEN];
    s.read_exact(&mut c2).await?;
    Ok(())
}

/// Plain handshake as the connecting side, the same one OBS uses towards Twitch/YouTube.
pub async fn handshake_client<S: AsyncRead + AsyncWrite + Unpin>(s: &mut S) -> io::Result<()> {
    let mut c0c1 = vec![3u8];
    c0c1.extend_from_slice(&random_block());
    s.write_all(&c0c1).await?;
    let mut s0s1s2 = vec![0u8; 1 + 2 * HANDSHAKE_LEN];
    s.read_exact(&mut s0s1s2).await?;
    s.write_all(&s0s1s2[1..1 + HANDSHAKE_LEN]).await?;
    Ok(())
}

pub fn command(stream_id: u32, values: &[amf0::Amf]) -> chunk::Message {
    chunk::Message { typ: chunk::MSG_CMD_AMF0, ts: 0, stream_id, payload: amf0::encode_all(values) }
}
