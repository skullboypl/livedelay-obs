//! One delay engine per platform ingest key, with shared Arm/Activate/Cut.

use crate::engine::{Mode, Shared, Status};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::watch;

/// Dropped OBS stock publish (Start Streaming still needs somewhere to go).
pub const IDLE_KEY: &str = "_idle";
/// Pre-1.0.6 Route OBS used this key and mapped it onto slot 0, so it raced with
/// `ld0` ("publisher busy") and Twitch got the stock encode (player #1000).
/// Treat it like idle: discard, never claim a real delay slot.
pub const LEGACY_KEY: &str = "livedelay";

pub struct Slot {
    pub name: String,
    pub ingest_key: String,
    pub shared: Arc<Shared>,
}

pub struct Hub {
    pub slots: Vec<Slot>,
    /// ingest_key → slot index (excludes idle).
    index: HashMap<String, usize>,
    /// Busy flag per ingest key currently publishing.
    busy: Mutex<HashMap<String, bool>>,
    /// Publish key that was not ld0… but was accepted on the first free slot.
    claimed: Mutex<HashMap<String, String>>,
    /// Stock OBS is publishing to `_idle` / legacy (media discarded, but OBS is live).
    idle_active: AtomicBool,
    /// Aggregated change counter for SSE / tray.
    pub state: watch::Sender<u64>,
}

impl Hub {
    /// `per_slot_bytes` is the buffer cap for each engine.
    pub fn new(dests: &[(String, String)], per_slot_bytes: usize) -> Arc<Self> {
        let (state, _) = watch::channel(0u64);
        let mut slots = Vec::new();
        let mut index = HashMap::new();
        for (i, (name, key)) in dests.iter().enumerate() {
            let ingest = if key.is_empty() { format!("ld{i}") } else { key.clone() };
            index.insert(ingest.clone(), i);
            slots.push(Slot {
                name: name.clone(),
                ingest_key: ingest,
                shared: Shared::new(per_slot_bytes),
            });
        }
        // Do NOT map LEGACY_KEY → slot 0 (that blocked ld0 and broke Twitch).
        Arc::new(Self {
            slots,
            index,
            busy: Mutex::new(HashMap::new()),
            claimed: Mutex::new(HashMap::new()),
            idle_active: AtomicBool::new(false),
            state,
        })
    }

    pub fn idle_active(&self) -> bool {
        self.idle_active.load(Ordering::Relaxed)
    }

    /// Call inside the tokio runtime so engine state changes refresh the dock.
    pub fn spawn_state_forwarders(self: &Arc<Self>) {
        for slot in &self.slots {
            let hub_state = self.state.clone();
            let mut rx = slot.shared.state.subscribe();
            tokio::spawn(async move {
                loop {
                    if rx.changed().await.is_err() {
                        break;
                    }
                    let _ = rx.borrow_and_update();
                    hub_state.send_modify(|v| *v += 1);
                }
            });
        }
    }

    pub fn slot(&self, i: usize) -> Option<&Slot> {
        self.slots.get(i)
    }

    pub fn shared_for_key(&self, key: &str) -> Option<Arc<Shared>> {
        let key = key.trim();
        if Self::is_idle_key(key) {
            return None;
        }
        self.index.get(key).map(|&i| self.slots[i].shared.clone())
    }

    pub fn is_idle_key(key: &str) -> bool {
        let k = key.trim();
        k.is_empty() || k == IDLE_KEY || k.eq_ignore_ascii_case(LEGACY_KEY)
    }

    fn map_key(&self, key: &str) -> String {
        let key = key.trim();
        if Self::is_idle_key(key) {
            return IDLE_KEY.to_string();
        }
        self.index.get(key).map(|i| self.slots[*i].ingest_key.clone()).unwrap_or_else(|| key.to_string())
    }

    /// Take the first platform slot when it is free. The publish key is remembered
    /// so release clears that slot instead of leaving it busy.
    fn claim_first_free(&self, key: &str) -> Option<Arc<Shared>> {
        let mut busy = self.busy.lock().unwrap();
        let slot = self.slots.first()?;
        let map_key = slot.ingest_key.clone();
        if busy.get(&map_key).copied().unwrap_or(false) {
            return None;
        }
        busy.insert(map_key.clone(), true);
        drop(busy);
        self.claimed.lock().unwrap().insert(key.to_string(), map_key);
        Some(slot.shared.clone())
    }

    /// Claim a publisher slot. `Ok(None)` = idle drop; `Ok(Some)` = real engine.
    pub fn try_claim(&self, key: &str) -> Result<Option<Arc<Shared>>, &'static str> {
        let key = key.trim();
        if Self::is_idle_key(key) {
            // The plugin discards `_idle`: its real encode arrives separately as ld0.
            // The standalone app has only this one publish, so it is the stream.
            if crate::ffi::host() != "obs" {
                if let Some(shared) = self.claim_first_free(key) {
                    log!("RTMP: key '{key}' — accepting it as the stream from this PC");
                    return Ok(Some(shared));
                }
            }
            // Never block OBS Start Streaming on the discarded stock publish —
            // OBS may reconnect while the previous idle TCP is still draining.
            let mut busy = self.busy.lock().unwrap();
            busy.insert(IDLE_KEY.to_string(), true);
            self.idle_active.store(true, Ordering::Relaxed);
            self.state.send_modify(|v| *v += 1);
            return Ok(None);
        }
        let Some(shared) = self.shared_for_key(key) else {
            // A program on this PC often publishes with its own key, not ld0.
            // One free slot takes that stream. A second publisher cannot steal a busy slot.
            if let Some(shared) = self.claim_first_free(key) {
                let map_key = self.slots[0].ingest_key.clone();
                log!("RTMP: ingest key is not {map_key} — accepting it as the stream from this PC");
                return Ok(Some(shared));
            }
            log!("RTMP: unknown ingest key — discarding (use ld0, ld1, …)");
            let mut busy = self.busy.lock().unwrap();
            busy.insert(format!("unknown:{key}"), true);
            self.idle_active.store(true, Ordering::Relaxed);
            self.state.send_modify(|v| *v += 1);
            return Ok(None);
        };
        let map_key = self.map_key(key);
        let mut busy = self.busy.lock().unwrap();
        if busy.get(&map_key).copied().unwrap_or(false) {
            return Err("busy");
        }
        busy.insert(map_key, true);
        Ok(Some(shared))
    }

    pub fn release(&self, key: &str) {
        let key = key.trim();
        let mut busy = self.busy.lock().unwrap();
        if let Some(slot_key) = self.claimed.lock().unwrap().remove(key) {
            busy.insert(slot_key, false);
            drop(busy);
            self.state.send_modify(|v| *v += 1);
            return;
        }
        if Self::is_idle_key(key) {
            busy.insert(IDLE_KEY.to_string(), false);
            self.idle_active.store(false, Ordering::Relaxed);
            drop(busy);
            self.state.send_modify(|v| *v += 1);
            return;
        }
        busy.insert(self.map_key(key), false);
        busy.insert(format!("unknown:{key}"), false);
        // Unknown keys were treated as idle for OBS Start Streaming.
        if !self.index.contains_key(key) {
            self.idle_active.store(false, Ordering::Relaxed);
            drop(busy);
            self.state.send_modify(|v| *v += 1);
        }
    }

    pub fn arm_all(&self, delay: Duration) -> Result<(), String> {
        if self.slots.is_empty() {
            return Err("need_arm".into());
        }
        for s in &self.slots {
            if let Err(e) = s.shared.with(|eng| eng.arm(delay)) {
                return Err(e.into());
            }
        }
        self.bump();
        Ok(())
    }

    pub fn disarm_all(&self) {
        for s in &self.slots {
            s.shared.with(|e| e.disarm());
        }
        self.bump();
    }

    pub fn activate_all(&self, now: Instant) -> Result<(), String> {
        if self.slots.is_empty() {
            return Err("need_arm".into());
        }
        let mut first_err: Option<String> = None;
        for s in &self.slots {
            if let Err(e) = s.shared.with(|e| e.activate(now)) {
                first_err.get_or_insert_with(|| e.to_string());
            }
        }
        self.bump();
        match first_err {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }

    pub fn cut_all(&self, now: Instant) {
        for s in &self.slots {
            s.shared.with(|e| e.cut(now));
        }
        self.bump();
    }

    pub fn cut_after_all(&self) {
        for s in &self.slots {
            s.shared.with(|e| e.cut_after_airs());
        }
        self.bump();
    }

    /// Aggregate status for the dock.
    pub fn status(&self, now: Instant) -> Status {
        if self.slots.is_empty() {
            return Status {
                mode: Mode::Live,
                input: false,
                delay_s: 0.0,
                buffer_s: 0.0,
                buffer_mb: 0.0,
                arm_delay_s: None,
                armed: false,
                cut_pending: false,
                in_kbps: 0.0,
            };
        }
        let statuses: Vec<Status> = self.slots.iter().map(|s| s.shared.eng.lock().unwrap().status(now)).collect();
        let primary = statuses.iter().find(|s| s.input).unwrap_or(&statuses[0]);
        let mut out = primary.clone();
        // Real media on ld0… OR stock OBS connected on _idle (dock must not say "no signal"
        // while OBS shows Stop Streaming).
        out.input = statuses.iter().any(|s| s.input) || self.idle_active();
        out.buffer_s = statuses.iter().map(|s| s.buffer_s).fold(0.0_f64, f64::max);
        out.buffer_mb = statuses.iter().map(|s| s.buffer_mb).sum();
        out.in_kbps = statuses.iter().map(|s| s.in_kbps).sum();
        out.mode = if statuses.iter().any(|s| s.mode == Mode::Delayed) { Mode::Delayed } else { Mode::Live };
        out.cut_pending = statuses.iter().any(|s| s.cut_pending);
        out.delay_s = statuses.iter().map(|s| s.delay_s).fold(0.0_f64, f64::max);
        out.arm_delay_s = statuses.iter().find_map(|s| s.arm_delay_s);
        out.armed = match out.arm_delay_s {
            Some(_) if out.input => statuses.iter().filter(|s| s.input).all(|s| s.armed),
            Some(_) => false,
            None => false,
        };
        out
    }

    pub fn bump(&self) {
        self.state.send_modify(|v| *v += 1);
    }
}
