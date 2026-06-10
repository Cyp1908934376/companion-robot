/// BCP protocol conformance tests.
///
/// Validates frame encode/decode, command serialization,
/// and edge cases against the BCP specification.

use bcp_core::{BcpFrame, BcpCodec, Command, Direction, LedMode, Capabilities, FaceExpression};

// ── Frame basics ────────────────────────────────────────────────

#[test]
fn frame_magic_byte() {
    let frame = BcpFrame::new(0);
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    assert_eq!(buf[0], 0xCB, "magic byte must be 0xCB");
    assert_eq!(len, 10, "empty frame = HEADER(8) + CRC(2)");
}

#[test]
fn frame_new_empty() {
    let frame = BcpFrame::new(42);
    assert_eq!(frame.seq_no, 42);
    assert_eq!(frame.cmd_count(), 0);
    assert!(frame.is_ack());
}

#[test]
fn roundtrip_empty_frame() {
    let frame = BcpFrame::new(1);
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, consumed) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(consumed, len);
    assert_eq!(decoded.seq_no, frame.seq_no);
    assert_eq!(decoded.commands.len(), 0);
}

#[test]
fn roundtrip_single_command() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::Heartbeat {
        status: 1, battery: 85, rssi: -45, task_id: 0,
    }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(decoded.commands.len(), 1);
    if let Command::Heartbeat { status, battery, rssi, .. } = &decoded.commands[0] {
        assert_eq!(*status, 1);
        assert_eq!(*battery, 85);
        assert_eq!(*rssi, -45);
    } else {
        panic!("expected Heartbeat");
    }
}

#[test]
fn roundtrip_multiple_commands() {
    let mut frame = BcpFrame::new(5);
    frame.push(Command::Heartbeat { status: 1, battery: 90, rssi: -30, task_id: 0 }).unwrap();
    frame.push(Command::Move { direction: Direction::Forward, speed: 80 }).unwrap();
    frame.push(Command::LedPattern { mode: LedMode::Breathing, speed: 50, r: 255, g: 0, b: 128 }).unwrap();

    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(decoded.commands.len(), 3);
    assert_eq!(decoded.seq_no, 5);
}

// ── Command coverage ────────────────────────────────────────────

#[test]
fn command_register() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::Register {
        capabilities: Capabilities::from_bits(0x000F),
        firmware_version: [1, 2, 3, 4],
    }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    if let Command::Register { capabilities, firmware_version } = &decoded.commands[0] {
        assert_eq!(*firmware_version, [1, 2, 3, 4]);
        assert!(capabilities.has(Capabilities::MOVEMENT));
    } else {
        panic!("expected Register");
    }
}

#[test]
fn command_motion_all_directions() {
    let dirs = vec![
        Direction::Forward, Direction::Backward,
        Direction::Left, Direction::Right,
        Direction::RotateLeft, Direction::RotateRight,
    ];
    for dir in dirs {
        let mut frame = BcpFrame::new(0);
        frame.push(Command::Move { direction: dir, speed: 100 }).unwrap();
        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
        if let Command::Move { direction, speed } = &decoded.commands[0] {
            assert_eq!(*direction, dir);
            assert_eq!(*speed, 100);
        } else {
            panic!("expected Move for {:?}", dir);
        }
    }
}

#[test]
fn command_sensor_data() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::EnvData {
        temp: 250, humi: 60, pressure: 1013, light: 500, air: 800,
    }).unwrap();
    frame.push(Command::ImuData {
        ax: 10, ay: -5, az: 980, gx: 0, gy: 1, gz: 0,
    }).unwrap();

    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(decoded.commands.len(), 2);
}

#[test]
fn command_events() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::MotionEvent { detect_type: 1, confidence: 85 }).unwrap();
    frame.push(Command::AudioEvent { event_type: 2, energy: 120 }).unwrap();
    frame.push(Command::TouchEvent { zone: 3, pressure: 200, state: 1 }).unwrap();
    frame.push(Command::Obstacle { direction: 1, distance: 45 }).unwrap();

    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(decoded.commands.len(), 4);
}

#[test]
fn command_led_all_modes() {
    let modes = vec![
        LedMode::Breathing, LedMode::Flow, LedMode::Blink, LedMode::Rainbow,
    ];
    for mode in modes {
        let mut frame = BcpFrame::new(0);
        frame.push(Command::LedPattern {
            mode, speed: 60, r: 128, g: 64, b: 200,
        }).unwrap();
        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
        if let Command::LedPattern { mode: m, r, g, b, .. } = &decoded.commands[0] {
            assert_eq!(*m, mode);
            assert_eq!(*r, 128);
            assert_eq!(*g, 64);
            assert_eq!(*b, 200);
        } else {
            panic!("expected LedPattern");
        }
    }
}

#[test]
fn command_task_status() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::TaskStatus { task_id: 42, status: 2, progress: 75 }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    if let Command::TaskStatus { task_id, status, progress } = &decoded.commands[0] {
        assert_eq!(*task_id, 42);
        assert_eq!(*status, 2);
        assert_eq!(*progress, 75);
    } else {
        panic!("expected TaskStatus");
    }
}

#[test]
fn command_stop_emergency() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::Stop { emergency: true }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    if let Command::Stop { emergency } = &decoded.commands[0] {
        assert!(*emergency);
    } else {
        panic!("expected Stop");
    }
}

#[test]
fn command_reg_ack() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::RegAck { short_id: 15, heartbeat_interval: 5000 }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    if let Command::RegAck { short_id, heartbeat_interval } = &decoded.commands[0] {
        assert_eq!(*short_id, 15);
        assert_eq!(*heartbeat_interval, 5000);
    } else {
        panic!("expected RegAck");
    }
}

// ── Edge cases ──────────────────────────────────────────────────

#[test]
fn seq_no_wraps_at_u16_max() {
    let frame = BcpFrame::new(65535);
    assert_eq!(frame.seq_no, 65535);
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(decoded.seq_no, 65535);
}

#[test]
fn bcp_frame_max_commands() {
    let mut frame = BcpFrame::new(0);
    for _ in 0..32 {
        frame.push(Command::LedOff).unwrap();
    }
    assert_eq!(frame.cmd_count(), 32);
    assert!(frame.push(Command::LedOff).is_err());
}

#[test]
fn bcp_frame_max_size_enforced() {
    let mut frame = BcpFrame::new(0);
    let big_data: heapless::Vec<u8, 255> = heapless::Vec::from_slice(&[0u8; 250]).unwrap();
    for _ in 0..4 {
        frame.push(Command::FaceCustom { frame_data: big_data.clone() }).unwrap();
    }
    frame.push(Command::LedOff).unwrap();
    let mut buf = [0u8; 2048];
    assert!(BcpCodec::encode(&frame, &mut buf).is_err());
}

#[test]
fn decode_rejects_invalid_magic() {
    let buf = vec![0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09];
    assert!(BcpCodec::decode(&buf).is_err());
}

#[test]
fn decode_rejects_truncated_frame() {
    let buf = vec![0xCB, 0x01, 0x10, 0x00, 0x01, 0x01];
    assert!(BcpCodec::decode(&buf).is_err());
}

#[test]
fn crc_detects_corruption() {
    let mut frame = BcpFrame::new(0);
    frame.push(Command::Stop { emergency: false }).unwrap();
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();

    let mid = len / 2;
    buf[mid] ^= 0xFF;

    assert!(BcpCodec::decode(&buf[..len]).is_err(), "CRC should detect corruption");
}

#[test]
fn zero_length_decode_fails() {
    assert!(BcpCodec::decode(&[]).is_err());
}

#[test]
fn face_expression_all_variants() {
    let exprs = vec![
        FaceExpression::Neutral, FaceExpression::Happy, FaceExpression::Sad,
        FaceExpression::Surprised, FaceExpression::Angry, FaceExpression::Confused,
    ];
    for expr in exprs {
        let mut frame = BcpFrame::new(0);
        frame.push(Command::FaceExpr { expr }).unwrap();
        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
        if let Command::FaceExpr { expr: e } = &decoded.commands[0] {
            assert_eq!(*e, expr);
        } else {
            panic!("expected FaceExpr");
        }
    }
}

#[test]
fn capabilities_bitmask() {
    let mut caps = Capabilities::new();
    assert!(!caps.has(Capabilities::MOVEMENT));
    caps.set(Capabilities::MOVEMENT);
    assert!(caps.has(Capabilities::MOVEMENT));
    caps.set(Capabilities::AUDIO);
    assert!(caps.has(Capabilities::AUDIO));
    assert_eq!(caps.bits(), Capabilities::MOVEMENT | Capabilities::AUDIO);
}
