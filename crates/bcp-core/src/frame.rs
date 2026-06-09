//! BCP frame structure.

use crate::command::Command;
use crate::constants::*;
use heapless::Vec;

/// A BCP protocol frame.
///
/// Contains a header (version, sequence number) and up to 32 commands.
/// Frames with zero commands are valid ACK frames.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BcpFrame {
    pub version: u8,
    pub seq_no: u16,
    pub commands: Vec<Command, MAX_COMMANDS_PER_FRAME>,
}

impl BcpFrame {
    /// Create a new empty frame with the given sequence number.
    pub fn new(seq_no: u16) -> Self {
        BcpFrame {
            version: VERSION,
            seq_no,
            commands: Vec::new(),
        }
    }

    /// Add a command to this frame. Returns `Err(command)` if the frame
    /// already contains the maximum 32 commands.
    pub fn push(&mut self, cmd: Command) -> Result<(), Command> {
        self.commands.push(cmd)
    }

    /// Total wire size of this frame: HEADER_LEN + sum(cmd wire sizes) + CRC_LEN.
    pub fn total_len(&self) -> usize {
        let cmds_len: usize = self.commands.iter().map(|c| c.wire_len()).sum();
        HEADER_LEN + cmds_len + CRC_LEN
    }

    /// Number of commands in this frame.
    pub fn cmd_count(&self) -> u8 {
        self.commands.len() as u8
    }

    /// Returns true if this is an ACK frame (no commands).
    pub fn is_ack(&self) -> bool {
        self.commands.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::command::Command;

    #[test]
    fn test_new_frame() {
        let frame = BcpFrame::new(42);
        assert_eq!(frame.version, VERSION);
        assert_eq!(frame.seq_no, 42);
        assert_eq!(frame.cmd_count(), 0);
        assert!(frame.is_ack());
    }

    #[test]
    fn test_total_len() {
        let mut frame = BcpFrame::new(1);
        frame.push(Command::Stop { emergency: false }).unwrap();
        // HEADER(8) + CMD_HEADER(3) + payload(1) + CRC(2) = 14
        assert_eq!(frame.total_len(), 14);
    }

    #[test]
    fn test_push_max() {
        let mut frame = BcpFrame::new(1);
        for _ in 0..32 {
            frame.push(Command::LedOff).unwrap();
        }
        assert_eq!(frame.cmd_count(), 32);
        assert!(frame.push(Command::LedOff).is_err());
    }
}
