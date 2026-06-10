/// Test vectors for BCP protocol — fixed known inputs/outputs.
///
/// These vectors can be used to verify firmware C implementation
/// produces identical output to the Rust reference implementation.

use bcp_core::{BcpFrame, BcpCodec, Command};

/// Verify heartbeat frame encoding is deterministic.
#[test]
fn test_vector_heartbeat() {
    let mut frame = BcpFrame::new(1);
    frame.push(Command::Heartbeat {
        status: 1, battery: 100, rssi: -20, task_id: 0,
    }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let hex = hex::encode(&buf[..len]);

    // Encode a second time, expect identical output
    let mut buf2 = [0u8; 1024];
    let len2 = BcpCodec::encode(&frame, &mut buf2).unwrap();
    let hex2 = hex::encode(&buf2[..len2]);
    assert_eq!(len, len2);
    assert_eq!(hex, hex2, "Heartbeat encoding must be deterministic");
}

/// Verify RegAck encoding is deterministic.
#[test]
fn test_vector_reg_ack() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::RegAck { short_id: 7, heartbeat_interval: 5000 }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();

    let mut buf2 = [0u8; 1024];
    let len2 = BcpCodec::encode(&frame, &mut buf2).unwrap();
    assert_eq!(hex::encode(&buf[..len]), hex::encode(&buf2[..len2]));
}

/// Empty frame: no commands, just header + CRC = 10 bytes.
#[test]
fn test_vector_empty_frame() {
    let frame = BcpFrame::new(0);
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    assert_eq!(len, 10);
    assert_eq!(buf[0], 0xCB);
    assert_eq!(buf[6], 0); // cmd_count = 0
}

/// Cross-check: encode with Rust, print hex for firmware test vectors.
/// Run with: cargo test -p protocol-tests -- --nocapture
#[test]
fn print_cross_check_vectors() {
    let mut frame = BcpFrame::new(1);
    frame.push(Command::Move { direction: bcp_core::Direction::Forward, speed: 128 }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let hex = hex::encode(&buf[..len]);
    // HEADER(8) + CMD_HDR(3) + MOVE(2) + CRC(2) = 15
    assert_eq!(len, 15);
    println!("MOVE(Forward,128) seq=1: {}", hex);
}
