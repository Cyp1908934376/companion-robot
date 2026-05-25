# Companion Robot 集群系统

基于 ESP32-S3 机器人终端 + Rust 主脑服务 + 移动端桥接APP 的分布式陪伴机器人集群系统。

## 特性

- **小型化**: 初代机器人30-40cm高，<800g，一手可托举
- **全向移动**: 麦克纳姆轮底盘，室内灵活穿梭
- **完整感知**: 视觉+音频+环境+触觉+避障
- **集群调度**: 50+机器人并发，智能任务分配
- **多通信模式**: WiFi直连 / BLE桥接 / 第三方API转发，自动切换
- **边缘计算**: 手机分担AI推理，简单意图<1s响应
- **自主充电**: 红外信标引导，低电量自动回充电站
- **多用户共享**: 家庭模型，权限隔离，儿童安全模式

## 系统架构

```
┌─────────────────────────────────────────────────────────┐
│                        主脑集群 (Rust)                    │
│  Gateway ← NATS → Scheduler / DeviceManager / AI Service │
└─────────────────────────┬───────────────────────────────┘
                          │ WiFi / WebSocket
         ┌────────────────┼────────────────┐
         │                │                │
    ┌────▼────┐     ┌────▼────┐     ┌────▼────┐
    │ Robot 1 │     │ Robot 2 │     │ Robot N │  (ESP32-S3)
    └────┬────┘     └────┬────┘     └────┬────┘
         │ BLE           │ BLE           │ BLE
    ┌────▼────┐     ┌────▼────┐     ┌────▼────┐
    │ 手机/平板│     │ 手机/平板│     │ 手机/平板│  (桥接+边缘计算)
    └─────────┘     └─────────┘     └─────────┘
```

## 技术栈

| 层级 | 技术 |
|------|------|
| 机器人MCU | ESP32-S3 (双核240MHz, WiFi+BLE, NPU) |
| 机器人OS | ESP-IDF v5.x (FreeRTOS) |
| 通信协议 | BCP (Bundle Command Protocol) — 自定义二进制帧 |
| 主脑语言 | Rust (tokio异步运行时) |
| 消息队列 | NATS JetStream |
| 数据库 | PostgreSQL + TimescaleDB |
| 移动端 | Android (Kotlin) / iOS (Swift) |
| 监控 | Prometheus + Grafana |

## 项目结构

```
companion-robot/
├── docs/                      — 架构文档 (12篇)
├── prompts/                   — AI实现提示词 (10篇)
├── crates/bcp-core/           — BCP协议栈 (no_std Rust)
├── firmware/                  — ESP32-S3 固件
├── services/                  — 主脑服务
│   ├── gateway/               — WebSocket网关
│   ├── device-manager/        — 设备管理
│   ├── scheduler/             — 任务调度
│   └── ai/                    — AI推理
├── mobile/                    — 移动端APP
│   ├── android/               — Android
│   └── ios/                   — iOS
└── tests/                     — 测试套件
```

## 演进路线

| 阶段 | 体型 | 关键能力 | BOM |
|------|------|----------|-----|
| Phase 1 MiniBot | 30-40cm | 全向底盘+摄像头+麦克风阵列+ToF避障 | ~386 RMB |
| Phase 2 CompanionBot | 50-60cm | 深度传感器+手臂+FSR触觉 | ~800 RMB |
| Phase 3 HomeBot | 80-100cm | 深度相机+激光雷达+机械臂 | ~2000 RMB |
| Phase 4 ProBot | 120cm+ | 双臂+GPU推理+全身触觉 | ~5000 RMB |

架构完全兼容，通过能力集注册自动适配不同阶段的机器人。

## 快速开始

### 开发环境

```bash
# 启动主脑服务
docker-compose up -d

# 编译固件
cd firmware && idf.py build

# 编译主脑
cd services && cargo build --release

# 运行测试
cd crates/bcp-core && cargo test
cd services && cargo test --all
```

### 文档导航

| 文档 | 内容 |
|------|------|
| [architecture.md](docs/architecture.md) | 总体架构、拓扑、通信模式 |
| [protocol.md](docs/protocol.md) | BCP协议完整规范 |
| [robot-firmware.md](docs/robot-firmware.md) | 固件架构、FreeRTOS任务 |
| [main-brain.md](docs/main-brain.md) | 主脑服务拆分 |
| [hardware-form-factor.md](docs/hardware-form-factor.md) | 硬件BOM、充电站设计 |
| [mobile-sdk.md](docs/mobile-sdk.md) | 移动端SDK、边缘计算 |
| [security.md](docs/security.md) | 认证、加密、权限 |
| [testing-strategy.md](docs/testing-strategy.md) | 测试策略、CI/CD |
| [error-handling.md](docs/error-handling.md) | 错误码、降级策略 |
| [performance-sla.md](docs/performance-sla.md) | SLA、容量规划 |
| [multi-user.md](docs/multi-user.md) | 多用户、家庭共享 |

## 核心协议 — BCP

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Magic(1B)  |   Version(1B) |        TotalLen(2B)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     SeqNo(2B) |  CmdCount(1B) |        Reserved(1B)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Commands...                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         CRC16(2B)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- Magic: `0xCB`
- 集束: 单帧最多32条指令
- CRC-16/CCITT校验
- 差量心跳: 无变化时仅3字节
- 免确认: 连续运动指令可跳过ACK

完整规范见 [protocol.md](docs/protocol.md)

## 许可证

MIT License
