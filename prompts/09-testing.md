# 测试套件实现提示词

## 角色

你是一个测试工程师。请根据以下架构文档实现完整的测试套件。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/testing-strategy.md` — 测试策略

## 实现要求

### 1. BCP协议一致性测试套件

创建一个跨平台共享的测试向量文件：

```json
// tests/protocol/vectors.json
{
  "vectors": [
    {
      "name": "heartbeat_frame",
      "description": "单条心跳指令帧",
      "frame": {
        "version": 1,
        "seq_no": 1,
        "commands": [
          {
            "cmd_id": "0x0001",
            "payload": "0164C80A00"
          }
        ]
      },
      "encoded_hex": "CB010F00010001000101010564C80A00XXXX",
      "notes": "CRC用XXXX表示,需根据实际计算填充"
    },
    {
      "name": "move_and_led_bundle",
      "description": "集束帧: 运动+LED",
      "frame": {
        "version": 1,
        "seq_no": 66,
        "commands": [
          {
            "cmd_id": "0x0101",
            "payload": "0080"
          },
          {
            "cmd_id": "0x0202",
            "payload": "0120FF0000"
          }
        ]
      }
    },
    {
      "name": "bad_magic",
      "description": "Magic错误,应返回BadMagic",
      "input_hex": "CC010F00010001000101010564C80A000000",
      "expected_error": "BadMagic"
    },
    {
      "name": "bad_crc",
      "description": "CRC错误,应返回BadCrc",
      "input_hex": "CB010F00010001000101010564C80A00FF00",
      "expected_error": "BadCrc"
    },
    {
      "name": "empty_frame",
      "description": "空帧(CmdCount=0)",
      "frame": {
        "version": 1,
        "seq_no": 1,
        "commands": []
      }
    },
    {
      "name": "max_commands",
      "description": "最大指令数(32条)",
      "frame": {
        "version": 1,
        "seq_no": 1,
        "commands": "repeat_32_times: { cmd_id: 0x0001, payload: \"\" }"
      }
    }
  ]
}
```

### 2. Rust单元测试 (主脑)

```rust
// tests/bcp_conformance.rs
use bcp_core::{BcpCodec, BcpFrame, Command};
use serde::Deserialize;

#[derive(Deserialize)]
struct TestVectors {
    vectors: Vec<TestVector>,
}

#[derive(Deserialize)]
struct TestVector {
    name: String,
    frame: Option<FrameSpec>,
    encoded_hex: Option<String>,
    input_hex: Option<String>,
    expected_error: Option<String>,
}

#[test]
fn test_conformance() {
    let json = include_str!("../tests/protocol/vectors.json");
    let vectors: TestVectors = serde_json::from_str(json).unwrap();

    for v in &vectors.vectors {
        if let Some(ref spec) = v.frame {
            // 编码测试
            let frame = spec.to_bcp_frame();
            let mut buf = [0u8; 1024];
            let len = BcpCodec::encode(&frame, &mut buf).unwrap();

            // 验证帧头
            assert_eq!(buf[0], 0xCB, "Magic mismatch in {}", v.name);
            assert_eq!(buf[1], 0x01, "Version mismatch in {}", v.name);

            // 解码往返测试
            let (decoded, _) = BcpCodec::decode(&buf[..len]).unwrap();
            assert_eq!(frame.commands.len(), decoded.commands.len(), "CmdCount mismatch in {}", v.name);
        }

        if let Some(ref hex) = v.input_hex {
            let buf = hex::decode(hex).unwrap();
            let result = BcpCodec::decode(&buf);
            if let Some(ref expected_err) = v.expected_error {
                assert!(result.is_err(), "Expected error in {}", v.name);
                assert_eq!(format!("{:?}", result.unwrap_err()), *expected_err);
            }
        }
    }
}
```

### 3. 负载测试脚本 (k6)

```javascript
// tests/load/gateway.js
import ws from 'k6/ws';
import { check, sleep } from 'k6';
import { Rate, Trend } from 'k6/metrics';

const msgLatency = new Trend('msg_latency');
const connectSuccess = new Rate('connect_success');

export const options = {
  stages: [
    { duration: '30s', target: 100 },
    { duration: '2m',  target: 500 },
    { duration: '5m',  target: 1000 },
    { duration: '30s', target: 0 },
  ],
  thresholds: {
    connect_success: ['rate>0.99'],
    msg_latency: ['p(95)<100'],
  },
};

function bcpHeartbeat(shortId) {
  const buf = new ArrayBuffer(15);
  const view = new DataView(buf);
  view.setUint8(0, 0xCB);    // Magic
  view.setUint8(1, 0x01);    // Version
  view.setUint16(2, 15, true); // TotalLen
  view.setUint16(4, 1, true);  // SeqNo
  view.setUint8(6, 1);        // CmdCount
  view.setUint8(7, 0);        // Reserved
  // Heartbeat command
  view.setUint16(8, 0x0001, true); // CmdID
  view.setUint8(10, 5);            // PayloadLen
  view.setUint8(11, 0x01);         // Status
  view.setUint8(12, 64);           // Battery
  view.setInt8(13, -45);           // RSSI
  // CRC placeholder
  view.setUint16(14, 0, true);
  return buf;
}

export default function () {
  const url = `ws://localhost:8080/ws?token=${__ENV.TOKEN}`;

  const res = ws.connect(url, function (socket) {
    socket.on('open', () => {
      connectSuccess.add(true);

      // 发送注册
      socket.sendBinary(bcpRegister());

      // 心跳循环
      socket.setInterval(() => {
        const start = Date.now();
        socket.sendBinary(bcpHeartbeat(__VU));
        socket.on('message', () => {
          msgLatency.add(Date.now() - start);
        });
      }, 5000);
    });

    socket.on('error', () => connectSuccess.add(false));
    socket.setTimeout(() => socket.close(), 120000);
  });

  check(res, { 'connected': (r) => r && r.status === 101 });
}
```

### 4. CI集成 (GitHub Actions)

```yaml
# .github/workflows/test.yml
name: Tests

on: [push, pull_request]

jobs:
  bcp-protocol:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
      - run: cd crates/bcp-core && cargo test

  main-brain:
    runs-on: ubuntu-latest
    services:
      nats:
        image: nats:latest
      redis:
        image: redis:7-alpine
      postgres:
        image: timescale/timescaledb:latest-pg16
        env:
          POSTGRES_PASSWORD: test
    steps:
      - uses: actions/checkout@v4
      - uses: dtolnay/rust-toolchain@stable
      - run: cd services && cargo test --all

  firmware:
    runs-on: ubuntu-latest
    container: espressif/idf:v5.2
    steps:
      - uses: actions/checkout@v4
      - run: cd firmware && idf.py build
      - run: cd firmware && idf.py -T components/bcp/test build
```

## 约束

- 测试向量JSON格式, 三个平台共享
- Rust: cargo test + testcontainers
- k6: 负载测试脚本
- CI: GitHub Actions
- 覆盖率目标: >80%
