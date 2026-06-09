//! Error types for BCP protocol operations.

/// Errors that can occur during BCP frame encoding/decoding.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BcpError {
    /// Frame does not start with magic byte 0xCB.
    BadMagic,
    /// Unsupported protocol version.
    BadVersion,
    /// Buffer does not contain a complete frame.
    Incomplete,
    /// CRC-16/CCITT checksum mismatch.
    CrcMismatch,
    /// Unknown or unsupported command ID.
    BadCmdId(u16),
    /// Command payload exceeds 255 bytes.
    PayloadTooLarge,
    /// Frame contains more than 32 commands.
    TooManyCommands,
    /// Output buffer is too small for the encoded frame.
    BufferFull,
    /// Encoded frame would exceed the 1024-byte maximum.
    FrameTooLarge,
    /// Frame total_len field is out of valid range (10..1024).
    BadFrameLength,
}

#[cfg(feature = "std")]
impl std::fmt::Display for BcpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BcpError::BadMagic => write!(f, "bad magic byte"),
            BcpError::BadVersion => write!(f, "unsupported protocol version"),
            BcpError::Incomplete => write!(f, "incomplete frame"),
            BcpError::CrcMismatch => write!(f, "CRC checksum mismatch"),
            BcpError::BadCmdId(id) => write!(f, "unknown command ID: 0x{:04X}", id),
            BcpError::PayloadTooLarge => write!(f, "payload exceeds 255 bytes"),
            BcpError::TooManyCommands => write!(f, "too many commands in frame (max 32)"),
            BcpError::BufferFull => write!(f, "output buffer too small"),
            BcpError::FrameTooLarge => write!(f, "frame exceeds 1024 bytes"),
            BcpError::BadFrameLength => write!(f, "frame length out of valid range"),
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for BcpError {}
