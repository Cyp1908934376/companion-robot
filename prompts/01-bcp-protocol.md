# BCP协议栈实现提示词

## 角色

你是一个嵌入式通信协议工程师。请根据以下架构文档实现BCP (Bundle Command Protocol) 协议栈。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/protocol.md` — BCP协议规范（帧结构、指令集、流控重传）
- `docs/error-handling.md` — 错误码定义

## 实现要求

### 1. Rust库 (`crates/bcp-core`)

创建一个no_std兼容的Rust库，固件和主脑共享同一份协议实现。

```
crates/bcp-core/
├── Cargo.toml
├── src/
│   ├── lib.rs          — 公共导出
│   ├── frame.rs        — 帧结构定义
│   ├── codec.rs        — 编解码器
│   ├── command.rs      — 指令枚举与payload定义
│   ├── error.rs        — 错误码
│   └── crc.rs          — CRC-16/CCITT
```

核心类型：

```rust
// frame.rs
pub struct BcpFrame {
    pub version: u8,
    pub seq_no: u16,
    pub commands: Vec<Command>,  // 或heapless::Vec for no_std
}

pub struct Command {
    pub cmd_id: u16,
    pub payload: Payload,
}

// command.rs — 指令枚举
pub enum Command {
    // 系统 (0x00xx)
    Heartbeat { status: u8, battery: u8, rssi: i8, task_id: u16 },
    Register { capabilities: Capabilities, firmware_version: [u8; 4] },
    RegAck { short_id: u16, heartbeat_interval: u16 },
    Ping { timestamp: u32 },
    Pong { timestamp: u32 },

    // 运动 (0x01xx)
    Move { direction: Direction, speed: u8 },
    MoveTo { x: i16, y: i16, speed: u8 },
    Stop { emergency: bool },
    ServoSet { id: u8, angle: u16 },

    // 表达 (0x02xx)
    LedSet { id: u16, r: u8, g: u8, b: u8 },
    LedPattern { pattern: u8, speed: u8, r: u8, g: u8, b: u8 },
    FaceExpression { expr: u8 },
    Speak { volume: u8, format: AudioFormat, data: Vec<u8> },

    // 感知 (0x03xx)
    EnvData { temp: i16, humi: u16, pressure: u32, light: u32, air: u16 },
    MotionEvent { detect_type: u8, confidence: u8 },
    AudioEvent { event_type: u8, energy: u16 },
    ImageSnapshot { format: u8, width: u16, height: u16, data: Vec<u8> },
    TouchEvent { zone: u8, pressure: u8, state: u8 },
    Obstacle { direction: u8, distance: u16 },

    // 集群 (0x04xx)
    TaskAssign { task_type: u8, priority: u8, params: Vec<u8> },
    TaskStatus { task_id: u16, status: u8, progress: u8 },
    PeerMsg { target: u16, data: Vec<u8> },
}

// codec.rs
pub struct BcpCodec;

impl BcpCodec {
    pub fn encode(frame: &BcpFrame, buf: &mut [u8]) -> Result<usize, BcpError>;
    pub fn decode(buf: &[u8]) -> Result<(BcpFrame, usize), BcpError>;
}
```

### 2. 测试向量

根据 `docs/protocol.md` §5.2 的示例，生成完整的测试向量：

```rust
#[test]
fn test_encode_move_and_led() {
    let frame = BcpFrame {
        version: 1,
        seq_no: 0x0042,
        commands: vec![
            Command::Move { direction: Direction::Forward, speed: 128 },
            Command::LedPattern { pattern: 1, speed: 0x20, r: 255, g: 0, b: 0 },
        ],
    };
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();

    // 验证帧头
    assert_eq!(buf[0], 0xCB);  // Magic
    assert_eq!(buf[1], 0x01);  // Version
    assert_eq!(buf[6], 2);     // CmdCount

    // 验证CRC
    let expected_crc = crc16_ccitt(&buf[..len-2]);
    let actual_crc = u16::from_le_bytes([buf[len-2], buf[len-1]]);
    assert_eq!(expected_crc, actual_crc);
}

#[test]
fn test_decode_bad_magic() {
    let buf = [0xCC, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
    let result = BcpCodec::decode(&buf);
    assert!(matches!(result, Err(BcpError::BadMagic)));
}

#[test]
fn test_roundtrip() {
    let frame = BcpFrame { /* ... */ };
    let mut buf = [0u8; 1024];
    let len = BcpCodec::encode(&frame, &mut buf).unwrap();
    let (decoded, consumed) = BcpCodec::decode(&buf[..len]).unwrap();
    assert_eq!(consumed, len);
    assert_eq!(frame.version, decoded.version);
    assert_eq!(frame.seq_no, decoded.seq_no);
    assert_eq!(frame.commands.len(), decoded.commands.len());
}
```

### 3. 差量心跳支持

实现 `docs/protocol.md` §8.1 的差量心跳：

```rust
pub struct HeartbeatDiffer {
    last: Option<HeartbeatState>,
}

impl HeartbeatDiffer {
    /// 返回差量编码，如果无变化返回None
    pub fn diff(&mut self, current: &HeartbeatState) -> Option<HeartbeatDelta> { ... }
}
```

### 4. 集束帧打包

实现优先级排序和批量打包：

```rust
pub struct FrameBuilder {
    commands: Vec<(Priority, Command)>,
}

impl FrameBuilder {
    pub fn push(&mut self, priority: Priority, cmd: Command);
    pub fn build(&mut self, max_len: usize) -> Option<BcpFrame>;
    // 按优先级排序，打包到max_len以内
}
```

## 约束

- `no_std` 兼容（使用 `heapless` 替代 `Vec`）
- 零拷贝解码（尽可能引用原始buffer）
- 所有多字节字段小端序
- CRC-16/CCITT 多项式: 0x1021
- 最大帧长度: 1024字节
- 最大指令数: 32条/帧
