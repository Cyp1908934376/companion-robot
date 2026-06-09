//! Delta heartbeat compression for bandwidth optimization.
//!
//! Typical savings: ~60% compared to sending full 7-byte heartbeat every time.

use crate::types::*;

/// Tracks the last transmitted heartbeat state and computes delta encodings.
pub struct HeartbeatDiffer {
    last: Option<HeartbeatState>,
}

impl HeartbeatDiffer {
    pub fn new() -> Self {
        HeartbeatDiffer { last: None }
    }

    /// Compare `current` against the last state and return a delta encoding.
    ///
    /// - First call always returns `Full`.
    /// - No fields changed → `NoChange` (3 bytes on wire instead of 7).
    /// - Only battery changed → `BatteryOnly` (3 bytes on wire).
    /// - Any other field changed → `Full` (7 bytes).
    pub fn diff(&mut self, current: &HeartbeatState) -> HeartbeatDelta {
        let prev = match &self.last {
            Some(p) => p,
            None => {
                self.last = Some(*current);
                return HeartbeatDelta::Full(*current);
            }
        };

        let delta = if prev.status == current.status
            && prev.rssi == current.rssi
            && prev.task_id == current.task_id
        {
            if prev.battery == current.battery {
                HeartbeatDelta::NoChange { short_id: current.short_id }
            } else {
                HeartbeatDelta::BatteryOnly { short_id: current.short_id, battery: current.battery }
            }
        } else {
            HeartbeatDelta::Full(*current)
        };

        self.last = Some(*current);
        delta
    }

    /// Reset the differ, forgetting the last state.
    pub fn reset(&mut self) {
        self.last = None;
    }
}

impl Default for HeartbeatDiffer {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state() -> HeartbeatState {
        HeartbeatState { short_id: 1, status: 0x01, battery: 85, rssi: -40, task_id: 0 }
    }

    #[test]
    fn test_first_heartbeat_is_full() {
        let mut differ = HeartbeatDiffer::new();
        let s = state();
        assert_eq!(differ.diff(&s), HeartbeatDelta::Full(s));
    }

    #[test]
    fn test_no_change() {
        let mut differ = HeartbeatDiffer::new();
        let s = state();
        differ.diff(&s);
        assert_eq!(differ.diff(&s), HeartbeatDelta::NoChange { short_id: 1 });
    }

    #[test]
    fn test_battery_only() {
        let mut differ = HeartbeatDiffer::new();
        let s1 = state();
        differ.diff(&s1);
        let s2 = HeartbeatState { battery: 80, ..s1 };
        assert_eq!(differ.diff(&s2), HeartbeatDelta::BatteryOnly { short_id: 1, battery: 80 });
    }

    #[test]
    fn test_status_change_triggers_full() {
        let mut differ = HeartbeatDiffer::new();
        let s1 = state();
        differ.diff(&s1);
        let s2 = HeartbeatState { status: 0x03, ..s1 };
        assert_eq!(differ.diff(&s2), HeartbeatDelta::Full(s2));
    }

    #[test]
    fn test_rssi_change_triggers_full() {
        let mut differ = HeartbeatDiffer::new();
        let s1 = state();
        differ.diff(&s1);
        let s2 = HeartbeatState { rssi: -50, ..s1 };
        assert_eq!(differ.diff(&s2), HeartbeatDelta::Full(s2));
    }

    #[test]
    fn test_reset() {
        let mut differ = HeartbeatDiffer::new();
        differ.diff(&state());
        differ.reset();
        // After reset, next should be full again
        let s = state();
        assert_eq!(differ.diff(&s), HeartbeatDelta::Full(s));
    }
}
