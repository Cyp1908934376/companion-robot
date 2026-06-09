//! Priority-based frame builder.
//!
//! Collects commands with priority annotations and packs them into frames
//! sorted by priority (emergency first), respecting the maximum frame size.

use crate::command::Command;
use crate::constants::*;
use crate::frame::BcpFrame;
use crate::types::Priority;
use heapless::Vec;

/// Builds BCP frames by collecting commands and packing them in priority order.
pub struct FrameBuilder {
    /// (priority, command) pairs
    entries: Vec<(Priority, Command), MAX_COMMANDS_PER_FRAME>,
}

impl FrameBuilder {
    pub fn new() -> Self {
        FrameBuilder {
            entries: Vec::new(),
        }
    }

    /// Enqueue a command with the given priority.
    ///
    /// Returns `Err(command)` if the builder already holds 32 commands.
    pub fn push(&mut self, priority: Priority, cmd: Command) -> Result<(), Command> {
        self.entries.push((priority, cmd)).map_err(|(_, c)| c)
    }

    /// Number of queued commands.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// True if no commands are queued.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Build a frame from queued commands, consuming those that fit.
    ///
    /// Commands are sorted by priority (Emergency first, BestEffort last).
    /// Packing stops when adding the next command would exceed `max_len`.
    /// Remaining commands stay in the queue.
    ///
    /// Returns `None` if the queue is empty.
    pub fn build(&mut self, seq_no: u16, max_len: usize) -> Option<BcpFrame> {
        if self.entries.is_empty() {
            return None;
        }

        // Sort by priority (insertion sort — max 32 entries, no_std compatible)
        let n = self.entries.len();
        for i in 1..n {
            let mut j = i;
            while j > 0 && self.entries[j - 1].0 > self.entries[j].0 {
                self.entries.swap(j - 1, j);
                j -= 1;
            }
        }

        let mut frame = BcpFrame::new(seq_no);
        let mut remaining: Vec<(Priority, Command), MAX_COMMANDS_PER_FRAME> = Vec::new();

        // Take all entries, iterate, keep non-fitting ones
        let all_entries = core::mem::replace(&mut self.entries, Vec::new());
        for (pri, cmd) in all_entries {
            let cmd_wire_len = cmd.wire_len();
            if frame.total_len() + cmd_wire_len <= max_len && frame.cmd_count() < 32 {
                frame.push(cmd).ok();
            } else {
                remaining.push((pri, cmd)).ok();
            }
        }

        self.entries = remaining;

        if frame.commands.is_empty() {
            None
        } else {
            Some(frame)
        }
    }

    /// Drain all remaining commands, returning them in priority order.
    pub fn drain_all(&mut self, seq_no: u16, max_len: usize) -> Vec<BcpFrame, 32> {
        let mut frames: Vec<BcpFrame, 32> = Vec::new();
        while let Some(frame) = self.build(seq_no, max_len) {
            frames.push(frame).ok();
        }
        frames
    }
}

impl Default for FrameBuilder {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::command::Command;
    use crate::types::*;

    #[test]
    fn test_builder_priority_order() {
        let mut builder = FrameBuilder::new();
        builder.push(Priority::BestEffort, Command::EnvData {
            temp: 25, humi: 50, pressure: 101325, light: 500, air: 400,
        }).unwrap();
        builder.push(Priority::Emergency, Command::Stop { emergency: true }).unwrap();
        builder.push(Priority::Expression, Command::LedOff).unwrap();
        builder.push(Priority::Motion, Command::Move {
            direction: Direction::Forward, speed: 128,
        }).unwrap();

        let frame = builder.build(1, 1024).unwrap();
        // Emergency first, then Motion, then Expression, then BestEffort
        assert!(matches!(frame.commands[0], Command::Stop { .. }));
        assert!(matches!(frame.commands[1], Command::Move { .. }));
        assert!(matches!(frame.commands[2], Command::LedOff));
        assert!(matches!(frame.commands[3], Command::EnvData { .. }));
        assert_eq!(builder.len(), 0);
    }

    #[test]
    fn test_builder_max_len() {
        let mut builder = FrameBuilder::new();
        // EnvData is 14 bytes payload + 3 header = 17 bytes on wire
        // Frame overhead = 8 (header) + 2 (CRC) = 10
        // So with max_len=27, only 17 bytes for commands = exactly 1 EnvData
        builder.push(Priority::BestEffort, Command::EnvData {
            temp: 25, humi: 50, pressure: 101325, light: 500, air: 400,
        }).unwrap();
        builder.push(Priority::BestEffort, Command::EnvData {
            temp: 26, humi: 51, pressure: 101326, light: 501, air: 401,
        }).unwrap();

        let frame = builder.build(1, 27).unwrap();
        assert_eq!(frame.commands.len(), 1);
        assert_eq!(builder.len(), 1); // second env data still in queue
    }

    #[test]
    fn test_builder_empty() {
        let mut builder: FrameBuilder = FrameBuilder::new();
        assert!(builder.build(1, 1024).is_none());
    }

    #[test]
    fn test_builder_drain_all() {
        let mut builder = FrameBuilder::new();
        for _ in 0..5 {
            builder.push(Priority::Motion, Command::Stop { emergency: false }).unwrap();
        }
        // Each Stop is 3 + 1 = 4 bytes. 5 * 4 = 20 + 10 overhead = 30.
        let frames = builder.drain_all(1, 1024);
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].commands.len(), 5);
    }
}
