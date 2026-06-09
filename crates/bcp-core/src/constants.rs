//! Protocol constants for BCP (Bundle Command Protocol).

pub const MAGIC: u8 = 0xCB;
pub const VERSION: u8 = 0x01;

pub const HEADER_LEN: usize = 8;
pub const CRC_LEN: usize = 2;
pub const MAX_FRAME_LEN: usize = 1024;
pub const MIN_FRAME_LEN: usize = HEADER_LEN + CRC_LEN; // 10 bytes (ACK frame)

pub const MAX_COMMANDS_PER_FRAME: usize = 32;
pub const MAX_PAYLOAD_LEN: usize = 255;
pub const CMD_HEADER_LEN: usize = 3; // CmdID(2) + PayloadLen(1)

pub const SLIDING_WINDOW_SIZE: u16 = 8;
pub const MAX_RETRIES: u8 = 3;
pub const INITIAL_RTO_MS: u16 = 200;
pub const DEFAULT_HEARTBEAT_MS: u16 = 5000;

// Instruction group prefixes
pub const CMD_SYSTEM: u16 = 0x0000;
pub const CMD_MOTION: u16 = 0x0100;
pub const CMD_EXPRESSION: u16 = 0x0200;
pub const CMD_PERCEPTION: u16 = 0x0300;
pub const CMD_CLUSTER: u16 = 0x0400;

// System command IDs
pub const CMD_HEARTBEAT: u16 = 0x0001;
pub const CMD_REGISTER: u16 = 0x0002;
pub const CMD_REG_ACK: u16 = 0x0003;
pub const CMD_PING: u16 = 0x0004;
pub const CMD_PONG: u16 = 0x0005;
pub const CMD_RESET: u16 = 0x0006;
pub const CMD_OTA_START: u16 = 0x0007;
pub const CMD_OTA_CHUNK: u16 = 0x0008;
pub const CMD_OTA_DONE: u16 = 0x0009;
pub const CMD_ERROR: u16 = 0x00FF;

// Motion command IDs
pub const CMD_MOVE: u16 = 0x0101;
pub const CMD_MOVE_TO: u16 = 0x0102;
pub const CMD_STOP: u16 = 0x0103;
pub const CMD_SERVO_SET: u16 = 0x0104;
pub const CMD_SERVO_BATCH: u16 = 0x0105;
pub const CMD_HEAD_PAN_TILT: u16 = 0x0106;

// Expression command IDs
pub const CMD_LED_SET: u16 = 0x0201;
pub const CMD_LED_PATTERN: u16 = 0x0202;
pub const CMD_LED_OFF: u16 = 0x0203;
pub const CMD_FACE_EXPR: u16 = 0x0204;
pub const CMD_FACE_CUSTOM: u16 = 0x0205;
pub const CMD_SPEAK: u16 = 0x0206;
pub const CMD_TTS_TEXT: u16 = 0x0207;

// Perception command IDs
pub const CMD_ENV_DATA: u16 = 0x0301;
pub const CMD_MOTION_EVENT: u16 = 0x0302;
pub const CMD_AUDIO_EVENT: u16 = 0x0303;
pub const CMD_AUDIO_STREAM: u16 = 0x0304;
pub const CMD_IMAGE_SNAPSHOT: u16 = 0x0305;
pub const CMD_DEPTH_DATA: u16 = 0x0306;
pub const CMD_TOUCH_EVENT: u16 = 0x0307;
pub const CMD_IMU_DATA: u16 = 0x0308;
pub const CMD_OBSTACLE: u16 = 0x0309;

// Cluster command IDs
pub const CMD_TASK_ASSIGN: u16 = 0x0401;
pub const CMD_TASK_STATUS: u16 = 0x0402;
pub const CMD_TASK_CANCEL: u16 = 0x0403;
pub const CMD_SWARM_FORM: u16 = 0x0404;
pub const CMD_PEER_MSG: u16 = 0x0405;

// Reserved flag bit
pub const FLAG_NO_ACK: u8 = 0x01;
