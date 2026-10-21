//! The delay engine: an in-memory buffer of FLV tags and the playhead that feeds every destination.
//!
//! Every tag remembers when it arrived. The playhead sends a tag once
//! `arrival + delay` has passed, so delay is just a wall-clock offset.
//! Activate and Cut move the playhead to a keyframe (back by the delay, or
//! forward to the live edge) and shift output timestamps so they keep counting
//! up: players see one continuous stream, never a reconnect.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::{broadcast, watch, Notify};

const MSG_AUDIO: u8 = 8;
const MSG_VIDEO: u8 = 9;
const MSG_DATA: u8 = 18;
/// Extra history kept beyond the delay, so there is always a keyframe at or before the target.
const RETAIN_MARGIN: Duration = Duration::from_secs(12);
/// Timestamp step inserted at a jump, roughly one frame.
const JUMP_GAP_MS: u64 = 33;

#[derive(Clone)]
pub struct Tag {
    seq: u64,
    ts: u64,
    arrival: Instant,
    kind: u8,
    key: bool,
    config: bool,
    data: Arc<Vec<u8>>,
}

#[derive(Clone, Debug)]
pub struct OutTag {
    pub kind: u8,
    pub ts: u64,
    pub key: bool,
    pub config: bool,
    pub data: Arc<Vec<u8>>,
}

#[derive(Clone, Debug)]
pub enum OutMsg {
    Tag(OutTag),
    /// The input stopped and everything buffered has been sent.
    End,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    Live,
    Delayed,
}

fn classify(kind: u8, d: &[u8]) -> (bool, bool) {
    let b0 = d.first().copied().unwrap_or(0);
    match kind {
        MSG_VIDEO if b0 & 0x80 != 0 => {
            // Enhanced RTMP: frame type in bits 4-6, packet type in the low nibble.
            let pkt = b0 & 0x0f;
            let frame = (b0 >> 4) & 7;
            (pkt == 0, frame == 1 && (pkt == 1 || pkt == 3))
        }
        MSG_VIDEO => {
            let avc_or_hevc = matches!(b0 & 0x0f, 7 | 12);
            let config = avc_or_hevc && d.get(1) == Some(&0);
            (config, !config && b0 >> 4 == 1)
        }
        MSG_AUDIO => {
            let fmt = b0 >> 4;
            let config = (fmt == 10 && d.get(1) == Some(&0)) || (fmt == 9 && b0 & 0x0f == 0);
            (config, false)
        }
        _ => (true, false), // script data (onMetaData)
    }
}

fn cfg_slot(t: &Tag) -> Option<usize> {
    match (t.kind, t.config) {
        (MSG_DATA, _) => Some(0),
        (MSG_VIDEO, true) => Some(1),
        (MSG_AUDIO, true) => Some(2),
        _ => None,
    }
}

pub struct Engine {
    tags: VecDeque<Tag>,
    bytes: usize,
    max_bytes: usize,
    next_seq: u64,

    // Input timestamps, made monotonic across wraps and OBS reconnects.
    publishing: bool,
    session_base: u64,
    first_raw: Option<u32>,
    last_raw: u32,
    wraps: u64,
    last_in_ts: u64,

    // Playhead.
    pub mode: Mode,
    output_active: bool,
    end_sent: bool,
    cursor: u64,
    delay: Duration,
    out_offset: i64,
    last_out_ts: u64,
    cut_after_seq: Option<u64>,
    /// Latest config tags sent, so a destination that (re)connects can start cleanly.
    sent_meta: Option<OutTag>,
    sent_video_cfg: Option<OutTag>,
    sent_audio_cfg: Option<OutTag>,

    /// Bytes received since the instant, for the input bitrate.
    in_window: (Instant, u64),
    in_kbps: f64,

    /// Config tags that fell out of the buffer, still needed by jumps to older-than-them points.
    evicted_cfg: [Option<Tag>; 3],

    pub arm_delay: Option<Duration>,

    out: broadcast::Sender<OutMsg>,
    state: watch::Sender<u64>,

    /// Undelayed copy of the input for /live-preview; only fed while someone watches.
    input_out: broadcast::Sender<OutMsg>,
    input_cfg: [Option<OutTag>; 3],
}

pub struct Shared {
    pub eng: Mutex<Engine>,
    pub wake: Notify,
    pub out: broadcast::Sender<OutMsg>,
    pub input: broadcast::Sender<OutMsg>,
    pub state: watch::Sender<u64>,
}

impl Shared {
    pub fn new(max_bytes: usize) -> Arc<Self> {
        let (out, _) = broadcast::channel(2048);
        let (input, _) = broadcast::channel(2048);
        let (state, _) = watch::channel(0);
        let mut engine = Engine::new(max_bytes, out.clone(), state.clone());
        engine.input_out = input.clone();
        Arc::new(Self {
            eng: Mutex::new(engine),
            wake: Notify::new(),
            out,
            input,
            state,
        })
    }

    pub fn with<R>(&self, f: impl FnOnce(&mut Engine) -> R) -> R {
        let r = f(&mut self.eng.lock().unwrap());
        self.wake.notify_one();
        r
    }
}

#[derive(Debug)]
pub struct Status {
    pub mode: Mode,
    pub input: bool,
    pub delay_s: f64,
    pub buffer_s: f64,
    pub buffer_mb: f64,
    pub arm_delay_s: Option<f64>,
    pub armed: bool,
    pub cut_pending: bool,
    /// Input bitrate from OBS over the last second.
    pub in_kbps: f64,
}

impl Engine {
    pub fn new(max_bytes: usize, out: broadcast::Sender<OutMsg>, state: watch::Sender<u64>) -> Self {
        Self {
            tags: VecDeque::new(),
            bytes: 0,
            max_bytes,
            next_seq: 0,
            publishing: false,
            session_base: 0,
            first_raw: None,
            last_raw: 0,
            wraps: 0,
            last_in_ts: 0,
            mode: Mode::Live,
            output_active: false,
            end_sent: false,
            cursor: 0,
            delay: Duration::ZERO,
            out_offset: 0,
            last_out_ts: 0,
            cut_after_seq: None,
            sent_meta: None,
            sent_video_cfg: None,
            sent_audio_cfg: None,
            evicted_cfg: [None, None, None],
            in_window: (Instant::now(), 0),
            in_kbps: 0.0,
            arm_delay: None,
            input_out: broadcast::channel(1).0,
            input_cfg: [None, None, None],
            out,
            state,
        }
    }

    fn changed(&self) {
        self.state.send_modify(|v| *v += 1);
    }

    pub fn begin_session(&mut self) {
        self.publishing = true;
        self.session_base = if self.next_seq == 0 { 0 } else { self.last_in_ts + JUMP_GAP_MS };
        self.first_raw = None;
        self.wraps = 0;
        self.changed();
    }

    pub fn end_session(&mut self) {
        self.publishing = false;
        self.changed();
    }

    fn expand_ts(&mut self, raw: u32) -> u64 {
        let first = *self.first_raw.get_or_insert(raw);
        if raw < self.last_raw && self.last_raw - raw > 0x8000_0000 {
            self.wraps += 1 << 32;
        }
        self.last_raw = raw;
        let ts = self.session_base + (self.wraps + raw as u64).saturating_sub(first as u64);
        self.last_in_ts = self.last_in_ts.max(ts);
        ts
    }

    pub fn push(&mut self, kind: u8, raw_ts: u32, data: Vec<u8>, now: Instant) {
        if !matches!(kind, MSG_AUDIO | MSG_VIDEO | MSG_DATA) || data.is_empty() {
            return;
        }
        let ts = self.expand_ts(raw_ts);
        let (config, key) = classify(kind, &data);
        self.bytes += data.len();
        self.in_window.1 += data.len() as u64;
        let el = now.saturating_duration_since(self.in_window.0);
        if el >= Duration::from_secs(1) {
            self.in_kbps = self.in_window.1 as f64 * 8.0 / 1000.0 / el.as_secs_f64();
            self.in_window = (now, 0);
        }
        let tag = Tag { seq: self.next_seq, ts, arrival: now, kind, key, config, data: Arc::new(data) };
        if let Some(slot) = cfg_slot(&tag) {
            self.input_cfg[slot] = Some(OutTag { kind, ts, key, config: true, data: tag.data.clone() });
        }
        if self.input_out.receiver_count() > 0 {
            let _ = self.input_out.send(OutMsg::Tag(OutTag { kind, ts, key, config, data: tag.data.clone() }));
        }
        self.next_seq += 1;
        self.tags.push_back(tag);
        if key && !self.output_active {
            self.start_output(now);
        }
        self.evict(now);
    }

    /// First keyframe of a stream (or after the output fully drained): start sending from it.
    fn start_output(&mut self, now: Instant) {
        let kf = self.tags.back().unwrap().clone();
        self.output_active = true;
        self.end_sent = false;
        self.jump_to(&kf, if self.mode == Mode::Delayed { self.delay } else { Duration::ZERO }, now);
        self.changed();
    }

    fn retain(&self) -> Duration {
        self.arm_delay.unwrap_or(Duration::ZERO).max(self.delay) + RETAIN_MARGIN
    }

    fn evict(&mut self, now: Instant) {
        let retain = self.retain();
        while let Some(front) = self.tags.front() {
            let old = now.saturating_duration_since(front.arrival) > retain;
            let over = self.bytes > self.max_bytes;
            let unsent = front.seq >= self.cursor && self.output_active;
            if !(old || over) || (unsent && !over) {
                break;
            }
            let t = self.tags.pop_front().unwrap();
            self.bytes -= t.data.len();
            if let Some(slot) = cfg_slot(&t) {
                self.evicted_cfg[slot] = Some(t);
            }
        }
    }

    fn index(&self, seq: u64) -> Option<usize> {
        let front = self.tags.front()?.seq;
        let i = seq.checked_sub(front)? as usize;
        (i < self.tags.len()).then_some(i)
    }

    /// Latest video keyframe that arrived at or before `at`.
    fn keyframe_before(&self, at: Instant) -> Option<&Tag> {
        self.tags.iter().rev().find(|t| t.kind == MSG_VIDEO && t.key && t.arrival <= at)
    }

    fn emit(&mut self, mut t: OutTag) {
        if t.kind == MSG_VIDEO && t.config {
            self.sent_video_cfg = Some(t.clone());
        } else if t.kind == MSG_AUDIO && t.config {
            self.sent_audio_cfg = Some(t.clone());
        } else if t.kind == MSG_DATA {
            self.sent_meta = Some(t.clone());
        }
        t.ts = t.ts.max(self.last_out_ts);
        self.last_out_ts = t.ts;
        let _ = self.out.send(OutMsg::Tag(t));
    }

    /// Moves the playhead to keyframe `kf` with the given delay and re-sends the
    /// codec config in force at that point, so the decoder never sees a mismatch.
    fn jump_to(&mut self, kf: &Tag, delay: Duration, now: Instant) {
        let next_out = if self.last_out_ts == 0 && self.sent_video_cfg.is_none() { 0 } else { self.last_out_ts + JUMP_GAP_MS };
        self.out_offset = next_out as i64 - kf.ts as i64;
        self.cursor = kf.seq;
        self.delay = if delay.is_zero() { Duration::ZERO } else { now.saturating_duration_since(kf.arrival) };
        let idx = self.index(kf.seq).unwrap_or(0);
        let mut cfg: [Option<Tag>; 3] = [None, None, None];
        for t in self.tags.range(..idx).rev() {
            let Some(slot) = cfg_slot(t) else { continue };
            if cfg[slot].is_none() {
                cfg[slot] = Some(t.clone());
            }
            if cfg.iter().all(Option::is_some) {
                break;
            }
        }
        for (slot, c) in cfg.iter_mut().enumerate() {
            if c.is_none() {
                *c = self.evicted_cfg[slot].clone();
            }
        }
        // Metadata is not repeated: players treat a second onMetaData as a new data stream.
        cfg[0] = None;
        for t in cfg.into_iter().flatten() {
            self.emit(OutTag { kind: t.kind, ts: next_out, key: false, config: true, data: t.data });
        }
    }

    /// Sends every tag that is due. Returns when the next one will be.
    pub fn pump(&mut self, now: Instant) -> Option<Instant> {
        if !self.output_active {
            return None;
        }
        loop {
            let front_seq = self.tags.front().map(|t| t.seq).unwrap_or(self.next_seq);
            self.cursor = self.cursor.max(front_seq);
            let Some(i) = self.index(self.cursor) else {
                if !self.publishing && !self.end_sent {
                    self.end_sent = true;
                    self.output_active = false;
                    let _ = self.out.send(OutMsg::End);
                    self.changed();
                }
                return None;
            };
            let t = &self.tags[i];
            let due = t.arrival + self.delay;
            if due > now {
                return Some(due);
            }
            let out = OutTag {
                kind: t.kind,
                ts: (t.ts as i64 + self.out_offset).max(0) as u64,
                key: t.key,
                config: t.config,
                data: t.data.clone(),
            };
            self.cursor += 1;
            self.emit(out);
            if self.cut_after_seq.is_some_and(|m| self.cursor > m) {
                self.cut_after_seq = None;
                self.cut(now);
            }
        }
    }

    pub fn arm(&mut self, delay: Duration) {
        self.arm_delay = Some(delay);
        self.changed();
    }

    pub fn disarm(&mut self) {
        self.arm_delay = None;
        self.changed();
    }

    /// True once the buffer reaches back far enough for the armed delay.
    pub fn is_armed(&self, now: Instant) -> bool {
        match (self.arm_delay, now.checked_sub(self.arm_delay.unwrap_or_default())) {
            (Some(_), Some(at)) => self.tags.front().is_some_and(|f| f.arrival <= at) && self.keyframe_before(at).is_some(),
            _ => false,
        }
    }

    pub fn activate(&mut self, now: Instant) -> Result<(), &'static str> {
        let d = self.arm_delay.ok_or("need_arm")?;
        if !self.output_active {
            self.mode = Mode::Delayed;
            self.delay = d;
            self.changed();
            return Ok(());
        }
        if !self.is_armed(now) {
            return Err("buffer_filling");
        }
        let kf = self.keyframe_before(now - d).ok_or("no_keyframe")?.clone();
        self.jump_to(&kf, d, now);
        self.mode = Mode::Delayed;
        self.cut_after_seq = None;
        self.changed();
        Ok(())
    }

    /// Jump straight to the live edge (the newest keyframe).
    pub fn cut(&mut self, now: Instant) {
        self.cut_after_seq = None;
        self.mode = Mode::Live;
        if self.output_active {
            if let Some(kf) = self.keyframe_before(now).cloned() {
                if kf.seq > self.cursor {
                    self.jump_to(&kf, Duration::ZERO, now);
                }
            }
        }
        self.delay = Duration::ZERO;
        self.changed();
    }

    /// Cut once what is live right now has reached viewers.
    pub fn cut_after_airs(&mut self) {
        if self.mode == Mode::Delayed && self.next_seq > 0 {
            self.cut_after_seq = Some(self.next_seq - 1);
            self.changed();
        }
    }

    /// Latest config tags seen on the input, for an undelayed preview.
    pub fn input_config(&self) -> Vec<OutTag> {
        self.input_cfg.iter().flatten().cloned().collect()
    }

    /// Config tags a freshly connected destination needs before the first keyframe.
    pub fn current_config(&self) -> Vec<OutTag> {
        [&self.sent_meta, &self.sent_video_cfg, &self.sent_audio_cfg].into_iter().flatten().cloned().collect()
    }

    pub fn status(&self, now: Instant) -> Status {
        Status {
            mode: self.mode,
            input: self.publishing,
            delay_s: self.delay.as_secs_f64(),
            buffer_s: self.tags.front().map(|f| now.saturating_duration_since(f.arrival).as_secs_f64()).unwrap_or(0.0),
            buffer_mb: self.bytes as f64 / 1_048_576.0,
            arm_delay_s: self.arm_delay.map(|d| d.as_secs_f64()),
            armed: self.is_armed(now),
            cut_pending: self.cut_after_seq.is_some(),
            in_kbps: if self.publishing && now.saturating_duration_since(self.in_window.0) < Duration::from_secs(3) { self.in_kbps } else { 0.0 },
        }
    }
}

/// Drives the playhead: sleeps until the next tag is due or new input arrives.
pub async fn run_pump(shared: Arc<Shared>) {
    let mut last_armed = false;
    loop {
        let now = Instant::now();
        let (next, armed) = {
            let mut e = shared.eng.lock().unwrap();
            let next = e.pump(now);
            let armed = e.is_armed(now);
            if armed != last_armed {
                e.changed();
            }
            (next, armed)
        };
        last_armed = armed;
        // Wake at least once a second so the "armed" flag flips on time.
        let deadline = next.unwrap_or(now + Duration::from_secs(1)).min(now + Duration::from_secs(1));
        tokio::select! {
            _ = shared.wake.notified() => {}
            _ = tokio::time::sleep_until(deadline.into()) => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn video(key: bool) -> Vec<u8> {
        vec![if key { 0x17 } else { 0x27 }, 1, 0, 0, 0, 0xAA]
    }

    fn setup() -> (Engine, broadcast::Receiver<OutMsg>) {
        let (out, rx) = broadcast::channel(100_000);
        let (state, _) = watch::channel(0);
        let mut e = Engine::new(1 << 30, out, state);
        e.begin_session();
        (e, rx)
    }

    fn drain(rx: &mut broadcast::Receiver<OutMsg>) -> Vec<OutTag> {
        let mut v = Vec::new();
        while let Ok(OutMsg::Tag(t)) = rx.try_recv() {
            v.push(t);
        }
        v
    }

    /// Feeds 30 fps video with a keyframe every second, from `from_s` to `to_s`.
    fn feed(e: &mut Engine, t0: Instant, from_s: u64, to_s: u64) {
        for f in from_s * 30..to_s * 30 {
            let ms = f * 1000 / 30;
            let now = t0 + Duration::from_millis(ms);
            e.push(MSG_VIDEO, ms as u32, video(f % 30 == 0), now);
            e.pump(now);
        }
    }

    #[test]
    fn activate_replays_from_a_keyframe_and_cut_returns_live_with_rising_timestamps() {
        let (mut e, mut rx) = setup();
        let t0 = Instant::now();
        e.push(MSG_VIDEO, 0, vec![0x17, 0, 0, 0, 0, 1], t0); // AVC config
        e.arm(Duration::from_secs(5));
        feed(&mut e, t0, 0, 10);
        let now = t0 + Duration::from_millis(9967);
        assert!(e.is_armed(now));
        e.activate(now).unwrap();
        assert_eq!(e.mode, Mode::Delayed);
        assert!(e.delay >= Duration::from_secs(5) && e.delay < Duration::from_secs(6));
        feed(&mut e, t0, 10, 20);
        let now = t0 + Duration::from_millis(19967);
        e.cut(now);
        e.pump(now);
        assert_eq!(e.mode, Mode::Live);
        feed(&mut e, t0, 20, 22);

        let out = drain(&mut rx);
        assert!(out.windows(2).all(|w| w[1].ts >= w[0].ts), "output timestamps never go back");
        // Two jumps (activate, cut) each re-send the AVC config.
        assert_eq!(out.iter().filter(|t| t.config).count(), 3);
    }

    #[test]
    fn cut_after_airs_waits_for_the_marked_tag() {
        let (mut e, mut rx) = setup();
        let t0 = Instant::now();
        e.arm(Duration::from_secs(3));
        feed(&mut e, t0, 0, 5);
        e.activate(t0 + Duration::from_secs(5)).unwrap();
        e.cut_after_airs();
        feed(&mut e, t0, 5, 7);
        assert_eq!(e.mode, Mode::Delayed, "marked frame has not aired yet");
        feed(&mut e, t0, 7, 10);
        assert_eq!(e.mode, Mode::Live);
        drain(&mut rx);
    }

    #[test]
    fn output_ends_after_buffer_drains() {
        let (mut e, mut rx) = setup();
        let t0 = Instant::now();
        e.arm(Duration::from_secs(2));
        feed(&mut e, t0, 0, 4);
        e.activate(t0 + Duration::from_secs(4)).unwrap();
        e.end_session();
        e.pump(t0 + Duration::from_secs(4));
        let mut ended = false;
        while let Ok(m) = rx.try_recv() {
            ended |= matches!(m, OutMsg::End);
        }
        assert!(!ended, "still sending the delayed tail");
        e.pump(t0 + Duration::from_secs(8));
        while let Ok(m) = rx.try_recv() {
            ended |= matches!(m, OutMsg::End);
        }
        assert!(ended);
    }
}
