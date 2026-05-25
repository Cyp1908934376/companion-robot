# 主脑网关服务实现提示词

## 角色

你是一个Rust后端工程师。请根据以下架构文档实现主脑的通信网关服务。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/main-brain.md` — 主脑服务架构
- `docs/protocol.md` — BCP协议规范
- `docs/performance-sla.md` — 性能SLA
- `docs/error-handling.md` — 错误处理

## 项目结构

```
services/gateway/
├── Cargo.toml
├── src/
│   ├── main.rs              — 启动入口, 配置加载
│   ├── config.rs            — 配置结构体
│   ├── ws_server.rs         — WebSocket服务端
│   ├── tcp_server.rs        — TCP服务端 (可选)
│   ├── bcp_codec.rs         — tokio_util::codec实现
│   ├── auth.rs              — 机器码认证
│   ├── conn_pool.rs         — 连接池管理
│   ├── nats_bridge.rs       — NATS消息桥接
│   ├── rate_limiter.rs      — 速率限制
│   ├── metrics.rs           — Prometheus指标
│   └── error.rs             — 错误类型
```

## 关键实现

### 1. BCP Codec (tokio_util)

```rust
use tokio_util::codec::{Decoder, Encoder};
use bytes::BytesMut;

pub struct BcpCodec;

impl Decoder for BcpCodec {
    type Item = BcpFrame;
    type Error = BcpError;

    fn decode(&mut self, src: &mut BytesMut) -> Result<Option<Self::Item>, Self::Error> {
        if src.len() < 8 {
            return Ok(None); // 等待更多数据
        }

        let total_len = u16::from_le_bytes([src[2], src[3]]) as usize;
        if src.len() < total_len {
            src.reserve(total_len - src.len());
            return Ok(None);
        }

        let frame_bytes = src.split_to(total_len);
        let frame = bcp_core::BcpCodec::decode(&frame_bytes)?;
        Ok(Some(frame))
    }
}

impl Encoder<BcpFrame> for BcpCodec {
    type Error = BcpError;

    fn encode(&mut self, item: BcpFrame, dst: &mut BytesMut) -> Result<(), Self::Error> {
        dst.reserve(1024);
        let len = bcp_core::BcpCodec::encode(&item, dst)?;
        dst.truncate(len);
        Ok(())
    }
}
```

### 2. WebSocket服务端

```rust
use tokio_tungstenite::accept_async;

pub async fn run_ws_server(config: Config) -> Result<()> {
    let listener = TcpListener::bind(&config.listen_addr).await?;
    let conn_pool = Arc::new(ConnPool::new());
    let nats = NatsClient::connect(&config.nats_url).await?;

    loop {
        let (stream, addr) = listener.accept().await?;
        let pool = conn_pool.clone();
        let nats = nats.clone();

        tokio::spawn(async move {
            // 认证
            let machine_id = authenticate(&stream).await?;

            // 分配短ID
            let short_id = pool.register(machine_id, addr).await?;

            // WebSocket握手
            let ws = accept_async(stream).await?;
            let framed = ws.framed(BcpCodec);

            // 双向转发
            let (mut sink, mut stream) = framed.split();

            // 下行: NATS → WebSocket
            let mut sub = nats.subscribe(&format!("robot.{}.cmd", short_id)).await?;
            let downlink = tokio::spawn(async move {
                while let Some(msg) = sub.next().await {
                    let frame = BcpCodec::decode(&msg.payload)?;
                    sink.send(frame).await?;
                }
            });

            // 上行: WebSocket → NATS
            while let Some(Ok(frame)) = stream.next().await {
                for cmd in &frame.commands {
                    let subject = match cmd {
                        Command::Heartbeat { .. } => format!("robot.{}.heartbeat", short_id),
                        Command::TaskStatus { .. } => format!("robot.{}.task_status", short_id),
                        _ => format!("robot.{}.event", short_id),
                    };
                    nats.publish(&subject, encode_cmd(cmd)?).await?;
                }
            }

            pool.unregister(short_id).await;
            downlink.abort();
        });
    }
}
```

### 3. 认证流程

```rust
pub async fn authenticate(stream: &TcpStream) -> Result<MachineId> {
    // 1. 发送CHALLENGE (32字节随机数)
    let challenge: [u8; 32] = rand::random();
    stream.write_all(&challenge).await?;

    // 2. 接收RESPONSE (签名+证书)
    let mut buf = [0u8; 1024];
    let n = stream.read(&mut buf).await?;
    let (signature, cert) = parse_response(&buf[..n])?;

    // 3. 验证证书链
    verify_cert_chain(&cert, &ROOT_CA)?;

    // 4. 验证签名
    verify_signature(&challenge, &signature, &cert.public_key)?;

    // 5. 检查黑名单
    if is_blacklisted(&cert.machine_id) {
        return Err(AuthError::Blacklisted);
    }

    Ok(cert.machine_id)
}
```

### 4. NATS桥接

```rust
// Subject映射
// 上行 (机器人→主脑):
//   robot.{short_id}.heartbeat → DeviceManager
//   robot.{short_id}.event     → AI服务/Scheduler
//   robot.{short_id}.sensor    → TimescaleDB
//
// 下行 (主脑→机器人):
//   robot.{short_id}.cmd       → Gateway → WebSocket
//   robot.{short_id}.task      → Gateway → WebSocket

pub struct NatsBridge {
    client: async_nats::Client,
}

impl NatsBridge {
    pub async fn forward_to_robot(&self, short_id: u16, frame: &BcpFrame) -> Result<()> {
        let subject = format!("robot.{}.cmd", short_id);
        let payload = bcp_core::encode_frame(frame)?;
        self.client.publish(subject, payload.into()).await?;
        Ok(())
    }
}
```

### 5. 速率限制

```rust
use governor::{RateLimiter, Quota, clock::DefaultClock};

pub struct ConnRateLimiter {
    per_conn: RateLimiter<NotKeyed, InMemoryState, DefaultClock>,
    global: RateLimiter<NotKeyed, InMemoryState, DefaultClock>,
}

impl ConnRateLimiter {
    pub fn new() -> Self {
        Self {
            per_conn: RateLimiter::direct(Quota::per_second(nonzero!(100u32))),
            global: RateLimiter::direct(Quota::per_second(nonzero!(10000u32))),
        }
    }

    pub fn check(&self) -> Result<(), BcpError> {
        self.per_conn.check().map_err(|_| BcpError::RateLimited)?;
        self.global.check().map_err(|_| BcpError::RateLimited)?;
        Ok(())
    }
}
```

### 6. Prometheus指标

```rust
use prometheus::{IntGauge, Histogram, register};

lazy_static! {
    static ref CONNECTIONS: IntGauge = register_int_gauge!("gateway_connections", "活跃连接数").unwrap();
    static ref CMD_LATENCY: Histogram = register_histogram!("gateway_cmd_latency_ms", "指令转发延迟").unwrap();
    static ref MSG_RATE: IntGauge = register_int_gauge!("gateway_msgs_per_second", "消息速率").unwrap();
}
```

## 约束

- tokio异步运行时
- 每连接一个tokio task
- 连接内存: ~2KB/连接
- 指标延迟: <1ms (NATS转发)
- 支持1000+并发连接
- 认证超时: 10s
- WebSocket心跳: 30s
- 速率限制: 100 msg/s/连接, 10000 msg/s全局
