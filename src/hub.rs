//! One delay engine per platform ingest key, with shared Arm/Activate/Cut.

use crate::engine::{Mode, Shared, Status};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::watch;

/// Dropped OBS stock publish (Start Streaming still needs somewhere to go).
pub const IDLE_KEY: &str = "_idle";
/// Older single-encode setups used this key; maps to slot 0 when present.
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
        if !slots.is_empty() {
            index.entry(LEGACY_KEY.to_string()).or_insert(0);
        }
        Arc::new(Self { slots, index, busy: Mutex::new(HashMap::new()), state })
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
        if key.is_empty() {
            return self.slots.first().map(|s| s.shared.clone());
        }
        self.index.get(key).map(|&i| self.slots[i].shared.clone())
    }

    pub fn is_idle_key(key: &str) -> bool {
        key.trim() == IDLE_KEY
    }

    fn map_key(&self, key: &str) -> String {
        let key = key.trim();
        if Self::is_idle_key(key) {
            return IDLE_KEY.to_string();
        }
        self.index.get(key).map(|i| self.slots[*i].ingest_key.clone()).unwrap_or_else(|| key.to_string())
    }

    /// Claim a publisher slot. `Ok(None)` = idle drop; `Ok(Some)` = real engine.
    pub fn try_claim(&self, key: &str) -> Result<Option<Arc<Shared>>, &'static str> {
        let key = key.trim();
        if Self::is_idle_key(key) {
            let mut busy = self.busy.lock().unwrap();
            if busy.get(IDLE_KEY).copied().unwrap_or(false) {
                return Err("busy");
            }
            busy.insert(IDLE_KEY.to_string(), true);
            return Ok(None);
        }
        let Some(shared) = self.shared_for_key(key) else {
            return Err("unknown");
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
        self.busy.lock().unwrap().insert(self.map_key(key), false);
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
        out.input = statuses.iter().any(|s| s.input);
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
