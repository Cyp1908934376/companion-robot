//! BCP (Bundle Command Protocol) core library.
//!
//! A `no_std`-compatible Rust implementation of the BCP binary protocol,
//! shared between ESP32-S3 firmware and the Rust main-brain services.
//!
//! # Features
//!
//! - `std` (default): enables `std::error::Error` impl for `BcpError`.
//!   Disable for `no_std` firmware builds.

#![cfg_attr(not(feature = "std"), no_std)]

pub mod constants;
pub mod error;
pub mod types;
pub mod crc;
pub mod command;
pub mod frame;
pub mod codec;
pub mod heartbeat;
pub mod builder;

// Re-exports for convenience
pub use codec::BcpCodec;
pub use command::Command;
pub use error::BcpError;
pub use frame::BcpFrame;
pub use types::*;
pub use heartbeat::HeartbeatDiffer;
pub use builder::FrameBuilder;
