//! BCP frame encoder/decoder.
//!
//! Implements the BCP wire format: 8-byte header, variable commands, 2-byte CRC-16/CCITT.
//! All multi-byte fields are little-endian.

use crate::command::Command;
use crate::constants::*;
use crate::crc::crc16_ccitt;
use crate::error::BcpError;
use crate::frame::BcpFrame;
use crate::types::*;
use heapless::Vec;

/// Stateless BCP codec for encoding and decoding frames.
pub struct BcpCodec;

impl BcpCodec {
    /// Encode a `BcpFrame` into `buf`, returning the number of bytes written.
    ///
    /// Returns `BcpError::BufferFull` if `buf` is too small.
    /// Returns `BcpError::FrameTooLarge` if the encoded frame would exceed `MAX_FRAME_LEN`.
    pub fn encode(frame: &BcpFrame, buf: &mut [u8]) -> Result<usize, BcpError> {
        let total_len = frame.total_len();

        if total_len > MAX_FRAME_LEN {
            return Err(BcpError::FrameTooLarge);
        }
        if buf.len() < total_len {
            return Err(BcpError::BufferFull);
        }

        // ── Header ──
        buf[0] = MAGIC;
        buf[1] = frame.version;
        buf[2] = (total_len as u16 & 0xFF) as u8;
        buf[3] = ((total_len as u16) >> 8) as u8;
        buf[4] = (frame.seq_no & 0xFF) as u8;
        buf[5] = ((frame.seq_no) >> 8) as u8;
        buf[6] = frame.cmd_count();
        buf[7] = 0x00; // reserved

        // ── Commands ──
        let mut pos = HEADER_LEN;
        for cmd in &frame.commands {
            let cmd_id = cmd.cmd_id();
            let payload_len = cmd.payload_len();

            buf[pos] = (cmd_id & 0xFF) as u8;
            buf[pos + 1] = ((cmd_id) >> 8) as u8;
            buf[pos + 2] = payload_len as u8;
            pos += CMD_HEADER_LEN;

            Self::encode_payload(cmd, &mut buf[pos..]);
            pos += payload_len;
        }

        // ── CRC ──
        let crc = crc16_ccitt(&buf[..total_len - CRC_LEN]);
        buf[total_len - 2] = (crc & 0xFF) as u8;
        buf[total_len - 1] = ((crc) >> 8) as u8;

        Ok(total_len)
    }

    /// Decode a BCP frame from `buf`, returning the decoded frame and bytes consumed.
    ///
    /// Uses zero-copy references where possible (payload data is copied into
    /// `heapless::Vec` since we need ownership for the `Command` enum).
    pub fn decode(buf: &[u8]) -> Result<(BcpFrame, usize), BcpError> {
        if buf.len() < MIN_FRAME_LEN {
            return Err(BcpError::Incomplete);
        }

        // ── Validate magic ──
        if buf[0] != MAGIC {
            return Err(BcpError::BadMagic);
        }

        // ── Validate version ──
        if buf[1] != VERSION {
            return Err(BcpError::BadVersion);
        }

        // ── Parse header ──
        let total_len = u16::from_le_bytes([buf[2], buf[3]]) as usize;
        if total_len < MIN_FRAME_LEN || total_len > MAX_FRAME_LEN {
            return Err(BcpError::BadFrameLength);
        }
        if buf.len() < total_len {
            return Err(BcpError::Incomplete);
        }

        let seq_no = u16::from_le_bytes([buf[4], buf[5]]);
        let cmd_count = buf[6];
        // buf[7] is reserved (NO_ACK flag)

        if cmd_count as usize > MAX_COMMANDS_PER_FRAME {
            return Err(BcpError::TooManyCommands);
        }

        // ── Validate CRC ──
        let expected_crc = u16::from_le_bytes([buf[total_len - 2], buf[total_len - 1]]);
        let computed_crc = crc16_ccitt(&buf[..total_len - CRC_LEN]);
        if expected_crc != computed_crc {
            return Err(BcpError::CrcMismatch);
        }

        // ── Parse commands ──
        let mut pos = HEADER_LEN;
        let cmd_end = total_len - CRC_LEN;
        let mut commands: Vec<Command, MAX_COMMANDS_PER_FRAME> = Vec::new();

        for _ in 0..cmd_count {
            if pos + CMD_HEADER_LEN > cmd_end {
                return Err(BcpError::Incomplete);
            }

            let cmd_id = u16::from_le_bytes([buf[pos], buf[pos + 1]]);
            let payload_len = buf[pos + 2] as usize;
            pos += CMD_HEADER_LEN;

            if pos + payload_len > cmd_end {
                return Err(BcpError::Incomplete);
            }

            let cmd = Self::decode_command(cmd_id, payload_len, &buf[pos..pos + payload_len])?;
            pos += payload_len;

            commands.push(cmd).map_err(|_| BcpError::TooManyCommands)?;
        }

        let frame = BcpFrame {
            version: VERSION,
            seq_no,
            commands,
        };

        Ok((frame, total_len))
    }

    /// Encode a single command's payload into `buf`.
    fn encode_payload(cmd: &Command, buf: &mut [u8]) {
        match cmd {
            Command::Heartbeat { status, battery, rssi, task_id } => {
                buf[0] = *status;
                buf[1] = *battery;
                buf[2] = *rssi as u8;
                buf[3] = (*task_id & 0xFF) as u8;
                buf[4] = ((*task_id) >> 8) as u8;
            }
            Command::Register { capabilities, firmware_version } => {
                let caps = capabilities.bits();
                buf[0] = (caps & 0xFF) as u8;
                buf[1] = ((caps) >> 8) as u8;
                buf[2..6].copy_from_slice(firmware_version);
            }
            Command::RegAck { short_id, heartbeat_interval } => {
                buf[0] = (*short_id & 0xFF) as u8;
                buf[1] = ((*short_id) >> 8) as u8;
                buf[2] = (*heartbeat_interval & 0xFF) as u8;
                buf[3] = ((*heartbeat_interval) >> 8) as u8;
            }
            Command::Ping { timestamp } | Command::Pong { timestamp } => {
                buf[0] = (*timestamp & 0xFF) as u8;
                buf[1] = ((*timestamp) >> 8) as u8;
                buf[2] = ((*timestamp) >> 16) as u8;
                buf[3] = ((*timestamp) >> 24) as u8;
            }
            Command::Reset { reason_code } => {
                buf[0] = *reason_code;
            }
            Command::OtaStart { size, md5 } => {
                buf[0] = (*size & 0xFF) as u8;
                buf[1] = ((*size) >> 8) as u8;
                buf[2] = ((*size) >> 16) as u8;
                buf[3] = ((*size) >> 24) as u8;
                buf[4..20].copy_from_slice(md5);
            }
            Command::OtaChunk { offset, data } => {
                buf[0] = (*offset & 0xFF) as u8;
                buf[1] = ((*offset) >> 8) as u8;
                buf[2] = ((*offset) >> 16) as u8;
                buf[3] = ((*offset) >> 24) as u8;
                buf[4..4 + data.len()].copy_from_slice(data);
            }
            Command::OtaDone => {}
            Command::Error { error_code, level, related_seq_no } => {
                buf[0] = (*error_code & 0xFF) as u8;
                buf[1] = ((*error_code) >> 8) as u8;
                buf[2] = *level;
                buf[3] = (*related_seq_no & 0xFF) as u8;
                buf[4] = ((*related_seq_no) >> 8) as u8;
            }
            Command::Move { direction, speed } => {
                buf[0] = *direction as u8;
                buf[1] = *speed;
            }
            Command::MoveTo { x, y, speed } => {
                buf[0] = (*x & 0xFF) as u8;
                buf[1] = ((*x) >> 8) as u8;
                buf[2] = (*y & 0xFF) as u8;
                buf[3] = ((*y) >> 8) as u8;
                buf[4] = *speed;
            }
            Command::Stop { emergency } => {
                buf[0] = if *emergency { 1 } else { 0 };
            }
            Command::ServoSet { id, angle } => {
                buf[0] = *id;
                buf[1] = (*angle & 0xFF) as u8;
                buf[2] = ((*angle) >> 8) as u8;
            }
            Command::ServoBatch { servos } => {
                buf[0] = servos.len() as u8;
                for (i, (id, angle)) in servos.iter().enumerate() {
                    let off = 1 + i * 3;
                    buf[off] = *id;
                    buf[off + 1] = (*angle & 0xFF) as u8;
                    buf[off + 2] = ((*angle) >> 8) as u8;
                }
            }
            Command::HeadPanTilt { pan, tilt } => {
                buf[0] = *pan;
                buf[1] = *tilt;
            }
            Command::LedSet { id, r, g, b } => {
                buf[0] = (*id & 0xFF) as u8;
                buf[1] = ((*id) >> 8) as u8;
                buf[2] = *r;
                buf[3] = *g;
                buf[4] = *b;
            }
            Command::LedPattern { mode, speed, r, g, b } => {
                buf[0] = *mode as u8;
                buf[1] = *speed;
                buf[2] = *r;
                buf[3] = *g;
                buf[4] = *b;
            }
            Command::LedOff => {}
            Command::FaceExpr { expr } => {
                buf[0] = *expr as u8;
            }
            Command::FaceCustom { frame_data } => {
                buf[..frame_data.len()].copy_from_slice(frame_data);
            }
            Command::Speak { volume, format, data } => {
                buf[0] = *volume;
                buf[1] = *format as u8;
                buf[2..2 + data.len()].copy_from_slice(data);
            }
            Command::TtsText { encoding, text } => {
                buf[0] = *encoding;
                buf[1..1 + text.len()].copy_from_slice(text);
            }
            Command::EnvData { temp, humi, pressure, light, air } => {
                buf[0] = (*temp & 0xFF) as u8;
                buf[1] = ((*temp) >> 8) as u8;
                buf[2] = (*humi & 0xFF) as u8;
                buf[3] = ((*humi) >> 8) as u8;
                buf[4] = (*pressure & 0xFF) as u8;
                buf[5] = ((*pressure) >> 8) as u8;
                buf[6] = ((*pressure) >> 16) as u8;
                buf[7] = ((*pressure) >> 24) as u8;
                buf[8] = (*light & 0xFF) as u8;
                buf[9] = ((*light) >> 8) as u8;
                buf[10] = ((*light) >> 16) as u8;
                buf[11] = ((*light) >> 24) as u8;
                buf[12] = (*air & 0xFF) as u8;
                buf[13] = ((*air) >> 8) as u8;
            }
            Command::MotionEvent { detect_type, confidence } => {
                buf[0] = *detect_type;
                buf[1] = *confidence;
            }
            Command::AudioEvent { event_type, energy } => {
                buf[0] = *event_type;
                buf[1] = (*energy & 0xFF) as u8;
                buf[2] = ((*energy) >> 8) as u8;
            }
            Command::AudioStream { encoding, data } => {
                buf[0] = *encoding;
                buf[1..1 + data.len()].copy_from_slice(data);
            }
            Command::ImageSnapshot { format, width, height, data } => {
                buf[0] = *format as u8;
                buf[1] = (*width & 0xFF) as u8;
                buf[2] = ((*width) >> 8) as u8;
                buf[3] = (*height & 0xFF) as u8;
                buf[4] = ((*height) >> 8) as u8;
                buf[5..5 + data.len()].copy_from_slice(data);
            }
            Command::DepthData { width, height, data } => {
                buf[0] = (*width & 0xFF) as u8;
                buf[1] = ((*width) >> 8) as u8;
                buf[2] = (*height & 0xFF) as u8;
                buf[3] = ((*height) >> 8) as u8;
                buf[4..4 + data.len()].copy_from_slice(data);
            }
            Command::TouchEvent { zone, pressure, state } => {
                buf[0] = *zone;
                buf[1] = *pressure;
                buf[2] = *state;
            }
            Command::ImuData { ax, ay, az, gx, gy, gz } => {
                buf[0] = (*ax & 0xFF) as u8;
                buf[1] = ((*ax) >> 8) as u8;
                buf[2] = (*ay & 0xFF) as u8;
                buf[3] = ((*ay) >> 8) as u8;
                buf[4] = (*az & 0xFF) as u8;
                buf[5] = ((*az) >> 8) as u8;
                buf[6] = (*gx & 0xFF) as u8;
                buf[7] = ((*gx) >> 8) as u8;
                buf[8] = (*gy & 0xFF) as u8;
                buf[9] = ((*gy) >> 8) as u8;
                buf[10] = (*gz & 0xFF) as u8;
                buf[11] = ((*gz) >> 8) as u8;
            }
            Command::Obstacle { direction, distance } => {
                buf[0] = *direction;
                buf[1] = (*distance & 0xFF) as u8;
                buf[2] = ((*distance) >> 8) as u8;
            }
            Command::TaskAssign { task_type, priority, params } => {
                buf[0] = *task_type;
                buf[1] = *priority;
                buf[2..2 + params.len()].copy_from_slice(params);
            }
            Command::TaskStatus { task_id, status, progress } => {
                buf[0] = (*task_id & 0xFF) as u8;
                buf[1] = ((*task_id) >> 8) as u8;
                buf[2] = *status;
                buf[3] = *progress;
            }
            Command::TaskCancel { task_id } => {
                buf[0] = (*task_id & 0xFF) as u8;
                buf[1] = ((*task_id) >> 8) as u8;
            }
            Command::SwarmForm { formation_id, coordinates } => {
                buf[0] = *formation_id;
                buf[1..1 + coordinates.len()].copy_from_slice(coordinates);
            }
            Command::PeerMsg { target, data } => {
                buf[0] = (*target & 0xFF) as u8;
                buf[1] = ((*target) >> 8) as u8;
                buf[2..2 + data.len()].copy_from_slice(data);
            }
        }
    }

    /// Decode a single command from its wire representation.
    fn decode_command(cmd_id: u16, payload_len: usize, payload: &[u8]) -> Result<Command, BcpError> {
        match cmd_id {
            CMD_HEARTBEAT => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                Ok(Command::Heartbeat {
                    status: payload[0],
                    battery: payload[1],
                    rssi: payload[2] as i8,
                    task_id: u16::from_le_bytes([payload[3], payload[4]]),
                })
            }
            CMD_REGISTER => {
                if payload_len < 6 { return Err(BcpError::Incomplete); }
                let caps = u16::from_le_bytes([payload[0], payload[1]]);
                let mut fw = [0u8; 4];
                fw.copy_from_slice(&payload[2..6]);
                Ok(Command::Register {
                    capabilities: Capabilities::from_bits(caps),
                    firmware_version: fw,
                })
            }
            CMD_REG_ACK => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                Ok(Command::RegAck {
                    short_id: u16::from_le_bytes([payload[0], payload[1]]),
                    heartbeat_interval: u16::from_le_bytes([payload[2], payload[3]]),
                })
            }
            CMD_PING => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                Ok(Command::Ping {
                    timestamp: u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]),
                })
            }
            CMD_PONG => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                Ok(Command::Pong {
                    timestamp: u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]),
                })
            }
            CMD_RESET => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                Ok(Command::Reset { reason_code: payload[0] })
            }
            CMD_OTA_START => {
                if payload_len < 20 { return Err(BcpError::Incomplete); }
                let size = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                let mut md5 = [0u8; 16];
                md5.copy_from_slice(&payload[4..20]);
                Ok(Command::OtaStart { size, md5 })
            }
            CMD_OTA_CHUNK => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                let offset = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                let mut data: Vec<u8, 255> = Vec::new();
                let chunk_len = payload_len - 4;
                if chunk_len > 255 { return Err(BcpError::PayloadTooLarge); }
                data.extend_from_slice(&payload[4..payload_len]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::OtaChunk { offset, data })
            }
            CMD_OTA_DONE => {
                Ok(Command::OtaDone)
            }
            CMD_ERROR => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                Ok(Command::Error {
                    error_code: u16::from_le_bytes([payload[0], payload[1]]),
                    level: payload[2],
                    related_seq_no: u16::from_le_bytes([payload[3], payload[4]]),
                })
            }
            CMD_MOVE => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                Ok(Command::Move {
                    direction: Direction::from_u8(payload[0]).ok_or(BcpError::BadCmdId(cmd_id))?,
                    speed: payload[1],
                })
            }
            CMD_MOVE_TO => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                Ok(Command::MoveTo {
                    x: i16::from_le_bytes([payload[0], payload[1]]),
                    y: i16::from_le_bytes([payload[2], payload[3]]),
                    speed: payload[4],
                })
            }
            CMD_STOP => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                Ok(Command::Stop { emergency: payload[0] != 0 })
            }
            CMD_SERVO_SET => {
                if payload_len < 3 { return Err(BcpError::Incomplete); }
                Ok(Command::ServoSet {
                    id: payload[0],
                    angle: u16::from_le_bytes([payload[1], payload[2]]),
                })
            }
            CMD_SERVO_BATCH => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                let count = payload[0] as usize;
                let mut servos: Vec<(u8, u16), 32> = Vec::new();
                for i in 0..count {
                    let off = 1 + i * 3;
                    if off + 3 > payload_len { return Err(BcpError::Incomplete); }
                    let id = payload[off];
                    let angle = u16::from_le_bytes([payload[off + 1], payload[off + 2]]);
                    servos.push((id, angle)).map_err(|_| BcpError::TooManyCommands)?;
                }
                Ok(Command::ServoBatch { servos })
            }
            CMD_HEAD_PAN_TILT => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                Ok(Command::HeadPanTilt { pan: payload[0], tilt: payload[1] })
            }
            CMD_LED_SET => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                Ok(Command::LedSet {
                    id: u16::from_le_bytes([payload[0], payload[1]]),
                    r: payload[2],
                    g: payload[3],
                    b: payload[4],
                })
            }
            CMD_LED_PATTERN => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                Ok(Command::LedPattern {
                    mode: LedMode::from_u8(payload[0]).ok_or(BcpError::BadCmdId(cmd_id))?,
                    speed: payload[1],
                    r: payload[2],
                    g: payload[3],
                    b: payload[4],
                })
            }
            CMD_LED_OFF => {
                Ok(Command::LedOff)
            }
            CMD_FACE_EXPR => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                Ok(Command::FaceExpr {
                    expr: FaceExpression::from_u8(payload[0]).ok_or(BcpError::BadCmdId(cmd_id))?,
                })
            }
            CMD_FACE_CUSTOM => {
                let mut frame_data: Vec<u8, 255> = Vec::new();
                frame_data.extend_from_slice(payload).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::FaceCustom { frame_data })
            }
            CMD_SPEAK => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                let volume = payload[0];
                let format = AudioFormat::from_u8(payload[1]).ok_or(BcpError::BadCmdId(cmd_id))?;
                let mut data: Vec<u8, 255> = Vec::new();
                data.extend_from_slice(&payload[2..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::Speak { volume, format, data })
            }
            CMD_TTS_TEXT => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                let encoding = payload[0];
                let mut text: Vec<u8, 255> = Vec::new();
                text.extend_from_slice(&payload[1..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::TtsText { encoding, text })
            }
            CMD_ENV_DATA => {
                if payload_len < 14 { return Err(BcpError::Incomplete); }
                Ok(Command::EnvData {
                    temp: i16::from_le_bytes([payload[0], payload[1]]),
                    humi: u16::from_le_bytes([payload[2], payload[3]]),
                    pressure: u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]),
                    light: u32::from_le_bytes([payload[8], payload[9], payload[10], payload[11]]),
                    air: u16::from_le_bytes([payload[12], payload[13]]),
                })
            }
            CMD_MOTION_EVENT => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                Ok(Command::MotionEvent { detect_type: payload[0], confidence: payload[1] })
            }
            CMD_AUDIO_EVENT => {
                if payload_len < 3 { return Err(BcpError::Incomplete); }
                Ok(Command::AudioEvent {
                    event_type: payload[0],
                    energy: u16::from_le_bytes([payload[1], payload[2]]),
                })
            }
            CMD_AUDIO_STREAM => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                let encoding = payload[0];
                let mut data: Vec<u8, 255> = Vec::new();
                data.extend_from_slice(&payload[1..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::AudioStream { encoding, data })
            }
            CMD_IMAGE_SNAPSHOT => {
                if payload_len < 5 { return Err(BcpError::Incomplete); }
                let format = ImageFormat::from_u8(payload[0]).ok_or(BcpError::BadCmdId(cmd_id))?;
                let width = u16::from_le_bytes([payload[1], payload[2]]);
                let height = u16::from_le_bytes([payload[3], payload[4]]);
                let mut data: Vec<u8, 255> = Vec::new();
                data.extend_from_slice(&payload[5..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::ImageSnapshot { format, width, height, data })
            }
            CMD_DEPTH_DATA => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                let width = u16::from_le_bytes([payload[0], payload[1]]);
                let height = u16::from_le_bytes([payload[2], payload[3]]);
                let mut data: Vec<u8, 255> = Vec::new();
                data.extend_from_slice(&payload[4..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::DepthData { width, height, data })
            }
            CMD_TOUCH_EVENT => {
                if payload_len < 3 { return Err(BcpError::Incomplete); }
                Ok(Command::TouchEvent { zone: payload[0], pressure: payload[1], state: payload[2] })
            }
            CMD_IMU_DATA => {
                if payload_len < 12 { return Err(BcpError::Incomplete); }
                Ok(Command::ImuData {
                    ax: i16::from_le_bytes([payload[0], payload[1]]),
                    ay: i16::from_le_bytes([payload[2], payload[3]]),
                    az: i16::from_le_bytes([payload[4], payload[5]]),
                    gx: i16::from_le_bytes([payload[6], payload[7]]),
                    gy: i16::from_le_bytes([payload[8], payload[9]]),
                    gz: i16::from_le_bytes([payload[10], payload[11]]),
                })
            }
            CMD_OBSTACLE => {
                if payload_len < 3 { return Err(BcpError::Incomplete); }
                Ok(Command::Obstacle {
                    direction: payload[0],
                    distance: u16::from_le_bytes([payload[1], payload[2]]),
                })
            }
            CMD_TASK_ASSIGN => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                let task_type = payload[0];
                let priority = payload[1];
                let mut params: Vec<u8, 255> = Vec::new();
                params.extend_from_slice(&payload[2..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::TaskAssign { task_type, priority, params })
            }
            CMD_TASK_STATUS => {
                if payload_len < 4 { return Err(BcpError::Incomplete); }
                Ok(Command::TaskStatus {
                    task_id: u16::from_le_bytes([payload[0], payload[1]]),
                    status: payload[2],
                    progress: payload[3],
                })
            }
            CMD_TASK_CANCEL => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                Ok(Command::TaskCancel {
                    task_id: u16::from_le_bytes([payload[0], payload[1]]),
                })
            }
            CMD_SWARM_FORM => {
                if payload_len < 1 { return Err(BcpError::Incomplete); }
                let formation_id = payload[0];
                let mut coordinates: Vec<u8, 255> = Vec::new();
                coordinates.extend_from_slice(&payload[1..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::SwarmForm { formation_id, coordinates })
            }
            CMD_PEER_MSG => {
                if payload_len < 2 { return Err(BcpError::Incomplete); }
                let target = u16::from_le_bytes([payload[0], payload[1]]);
                let mut data: Vec<u8, 255> = Vec::new();
                data.extend_from_slice(&payload[2..]).map_err(|_| BcpError::PayloadTooLarge)?;
                Ok(Command::PeerMsg { target, data })
            }
            _ => Err(BcpError::BadCmdId(cmd_id)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crc::crc16_ccitt;

    /// Reference frame from docs/protocol.md §5.2:
    /// Header:  CB 01 (total_len LE) (seq_no LE) 02 00
    /// Cmd1:   01 01 02 00 80  (MOVE Forward, speed=128)
    /// Cmd2:   02 02 05 01 20 FF 00 00  (LED_PATTERN Flow, speed=32, red)
    /// CRC:    2 bytes
    ///
    /// Total = 8 + 5 + 8 + 2 = 23 bytes
    #[test]
    fn test_encode_move_and_led() {
        let mut frame = BcpFrame::new(0x0042);
        frame.push(Command::Move { direction: Direction::Forward, speed: 128 }).unwrap();
        frame.push(Command::LedPattern {
            mode: LedMode::Flow,
            speed: 0x20,
            r: 255,
            g: 0,
            b: 0,
        }).unwrap();

        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        let encoded = &buf[..len];

        assert_eq!(len, 23, "total frame length");

        // Header
        assert_eq!(encoded[0], 0xCB, "magic");
        assert_eq!(encoded[1], 0x01, "version");
        assert_eq!(u16::from_le_bytes([encoded[2], encoded[3]]), 23, "total_len");
        assert_eq!(u16::from_le_bytes([encoded[4], encoded[5]]), 0x0042, "seq_no");
        assert_eq!(encoded[6], 2, "cmd_count");
        assert_eq!(encoded[7], 0x00, "reserved");

        // Command 1: MOVE (0x0101)
        let cmd1_off = 8;
        assert_eq!(u16::from_le_bytes([encoded[cmd1_off], encoded[cmd1_off + 1]]), 0x0101, "cmd1 id");
        assert_eq!(encoded[cmd1_off + 2], 2, "cmd1 payload len");
        assert_eq!(encoded[cmd1_off + 3], 0x00, "direction=Forward");
        assert_eq!(encoded[cmd1_off + 4], 0x80, "speed=128");

        // Command 2: LED_PATTERN (0x0202)
        let cmd2_off = 13;
        assert_eq!(u16::from_le_bytes([encoded[cmd2_off], encoded[cmd2_off + 1]]), 0x0202, "cmd2 id");
        assert_eq!(encoded[cmd2_off + 2], 5, "cmd2 payload len");
        assert_eq!(encoded[cmd2_off + 3], 0x01, "mode=Flow");
        assert_eq!(encoded[cmd2_off + 4], 0x20, "speed=32");
        assert_eq!(encoded[cmd2_off + 5], 0xFF, "R=255");
        assert_eq!(encoded[cmd2_off + 6], 0x00, "G=0");
        assert_eq!(encoded[cmd2_off + 7], 0x00, "B=0");

        // CRC
        let computed = crc16_ccitt(&encoded[..len - 2]);
        let stored = u16::from_le_bytes([encoded[len - 2], encoded[len - 1]]);
        assert_eq!(computed, stored, "CRC matches");
    }

    #[test]
    fn test_decode_bad_magic() {
        let buf = [0xCC, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
        assert!(matches!(BcpCodec::decode(&buf), Err(BcpError::BadMagic)));
    }

    #[test]
    fn test_decode_bad_version() {
        let buf = [0xCB, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
        assert!(matches!(BcpCodec::decode(&buf), Err(BcpError::BadVersion)));
    }

    #[test]
    fn test_decode_truncated() {
        let buf = [0xCB, 0x01, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00]; // claims 32 bytes
        assert!(matches!(BcpCodec::decode(&buf), Err(BcpError::Incomplete)));
    }

    #[test]
    fn test_crc_mismatch() {
        // Build a valid frame, then corrupt the CRC
        let mut frame = BcpFrame::new(1);
        frame.push(Command::Stop { emergency: false }).unwrap();
        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        // Corrupt CRC byte
        buf[len - 1] ^= 0xFF;
        assert!(matches!(BcpCodec::decode(&buf[..len]), Err(BcpError::CrcMismatch)));
    }

    #[test]
    fn test_roundtrip_move_and_led() {
        let mut frame = BcpFrame::new(0x0042);
        frame.push(Command::Move { direction: Direction::Forward, speed: 128 }).unwrap();
        frame.push(Command::LedPattern {
            mode: LedMode::Flow,
            speed: 0x20,
            r: 255,
            g: 0,
            b: 0,
        }).unwrap();

        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        let (decoded, consumed) = BcpCodec::decode(&buf[..len]).unwrap();

        assert_eq!(consumed, len);
        assert_eq!(decoded.version, frame.version);
        assert_eq!(decoded.seq_no, frame.seq_no);
        assert_eq!(decoded.commands.len(), frame.commands.len());
        assert_eq!(decoded.commands[0], frame.commands[0]);
        assert_eq!(decoded.commands[1], frame.commands[1]);
    }

    #[test]
    fn test_roundtrip_all_commands() {
        // Test one command from each group
        let test_cases: std::vec::Vec<Command> = std::vec![
            Command::Heartbeat { status: 0x01, battery: 85, rssi: -40, task_id: 7 },
            Command::Register { capabilities: Capabilities::from_bits(0x000F), firmware_version: [0, 1, 0, 0] },
            Command::RegAck { short_id: 42, heartbeat_interval: 5000 },
            Command::Ping { timestamp: 12345678 },
            Command::Pong { timestamp: 87654321 },
            Command::Reset { reason_code: 1 },
            Command::OtaStart { size: 65536, md5: [0xAA; 16] },
            Command::OtaDone,
            Command::Error { error_code: 0x0101, level: 2, related_seq_no: 5 },
            Command::Move { direction: Direction::Backward, speed: 64 },
            Command::MoveTo { x: 100, y: -50, speed: 200 },
            Command::Stop { emergency: true },
            Command::ServoSet { id: 1, angle: 90 },
            Command::HeadPanTilt { pan: 45, tilt: 30 },
            Command::LedSet { id: 3, r: 0, g: 255, b: 0 },
            Command::LedPattern { mode: LedMode::Rainbow, speed: 64, r: 255, g: 255, b: 255 },
            Command::LedOff,
            Command::FaceExpr { expr: FaceExpression::Happy },
            Command::EnvData { temp: 250, humi: 60, pressure: 101325, light: 1024, air: 500 },
            Command::MotionEvent { detect_type: 2, confidence: 90 },
            Command::AudioEvent { event_type: 1, energy: 500 },
            Command::TouchEvent { zone: 3, pressure: 100, state: 1 },
            Command::Obstacle { direction: 1, distance: 50 },
            Command::TaskAssign { task_type: 1, priority: 5, params: Vec::new() },
            Command::TaskStatus { task_id: 10, status: 1, progress: 75 },
            Command::TaskCancel { task_id: 10 },
            Command::PeerMsg { target: 5, data: Vec::new() },
        ];

        for cmd in test_cases {
            let mut frame = BcpFrame::new(1);
            frame.push(cmd.clone()).unwrap();
            let mut buf = [0u8; 1024];
            let len = BcpCodec::encode(&frame, &mut buf).unwrap();
            let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
            assert_eq!(decoded.commands.len(), 1, "roundtrip for {:?}", cmd);
            assert_eq!(decoded.commands[0], cmd, "roundtrip mismatch for {:?}", cmd);
        }
    }

    #[test]
    fn test_ack_frame() {
        let frame = BcpFrame::new(42); // empty = ACK
        let mut buf = [0u8; 1024];
        let len = BcpCodec::encode(&frame, &mut buf).unwrap();
        assert_eq!(len, 10); // HEADER(8) + CRC(2)
        assert_eq!(buf[6], 0); // cmd_count = 0

        let (decoded, consumed) = BcpCodec::decode(&buf[..len]).unwrap();
        assert_eq!(consumed, 10);
        assert_eq!(decoded.seq_no, 42);
        assert!(decoded.is_ack());
    }

    #[test]
    fn test_frame_too_large() {
        // Create a frame that exceeds 1024 bytes
        let mut frame = BcpFrame::new(1);
        // Each MOVE is 5 bytes on wire. 32 max commands.
        // Max payload bytes = 32 * 255 = 8160... but each cmd
        // only has cmd_header(3) + payload. Let's pack with big payloads.
        let big_data: Vec<u8, 255> = Vec::from_slice(&[0u8; 250]).unwrap();
        for _ in 0..4 {
            frame.push(Command::FaceCustom { frame_data: big_data.clone() }).unwrap();
        }
        // 8 + 4*(3+250) + 2 = 8 + 1012 + 2 = 1022 — fits
        assert!(frame.total_len() <= 1024);

        // Adding one more small cmd might push it over
        frame.push(Command::LedOff).unwrap();
        // 1022 + 3 + 0 = 1025 — over
        let mut buf = [0u8; 2048];
        assert!(matches!(BcpCodec::encode(&frame, &mut buf), Err(BcpError::FrameTooLarge)));
    }

    #[test]
    fn test_buffer_full() {
        let frame = BcpFrame::new(1); // ACK = 10 bytes
        let mut buf = [0u8; 5]; // too small
        assert!(matches!(BcpCodec::encode(&frame, &mut buf), Err(BcpError::BufferFull)));
    }

    #[test]
    fn test_decode_bad_cmd_id() {
        // Manually craft a frame with an unknown CmdID
        // Header: magic, version=1, total_len=14, seq_no=1, cmd_count=1
        let mut buf = [0u8; 14];
        buf[0] = 0xCB;
        buf[1] = 0x01;
        buf[2] = 14;
        buf[3] = 0;
        buf[4] = 1;
        buf[5] = 0;
        buf[6] = 1; // 1 command
        buf[7] = 0;
        // Command: CmdID=0xFFFF (unknown), payload_len=1, payload=[0x00]
        buf[8] = 0xFF;
        buf[9] = 0xFF;
        buf[10] = 1;
        buf[11] = 0;
        // CRC over bytes 0..11
        let crc = crc16_ccitt(&buf[..12]);
        buf[12] = (crc & 0xFF) as u8;
        buf[13] = ((crc) >> 8) as u8;

        assert!(matches!(BcpCodec::decode(&buf), Err(BcpError::BadCmdId(0xFFFF))));
    }
}
