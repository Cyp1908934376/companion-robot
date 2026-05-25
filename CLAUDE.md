# Companion Robot 集群系统

## 项目概述

这是一个基于 ESP32-S3 机器人终端 + Rust 主脑服务 + 移动端桥接APP 的分布式陪伴机器人集群系统。

- 初代机器人: 30-40cm高, <800g, 一手可托举, 麦克纳姆轮全向移动
- 集群规模: 50+机器人并发调度
- 感知能力: 视觉+音频+环境+触觉+避障

## 架构文档

所有架构设计文档在 `docs/` 目录：

```
docs/
├── architecture.md          — 总体架构(拓扑、三层模型、通信模式、集群管理)
├── protocol.md              — BCP集束指令协议(二进制帧、指令集、流控重传)
├── robot-firmware.md        — ESP32-S3固件(FreeRTOS任务、感知子系统、电源管理)
├── main-brain.md            — Rust主脑服务(网关/设备管理/调度器/AI推理)
├── security.md              — 安全设计(机器码认证、TLS、权限模型)
├── hardware-form-factor.md  — 硬件形态+充电站(BOM、结构、演进路线)
├── optimization-roadmap.md  — 优化清单(电源、定位、避碰、离线能力)
├── mobile-sdk.md            — 移动端SDK+边缘计算(桥接、本地AI、资源管理)
├── testing-strategy.md      — 测试策略(单元/HIL/集成/负载/CI)
├── error-handling.md        — 错误处理(错误码、降级、熔断、恢复)
├── performance-sla.md       — 性能SLA(延迟、吞吐、容量规划)
└── multi-user.md            — 多用户共享(家庭模型、权限、儿童安全、数据隔离)
```

## 技术栈

| 层级 | 技术 |
|------|------|
| 机器人MCU | ESP32-S3-WROOM-1 N16R8 (双核240MHz, 512KB SRAM + 8MB PSRAM, WiFi+BLE) |
| 机器人OS | ESP-IDF v5.x (FreeRTOS) |
| 通信协议 | BCP (Bundle Command Protocol) — 自定义二进制帧, 最大1024B |
| 主脑语言 | Rust (tokio异步) |
| 通信网关 | tokio-tungstenite (WebSocket) |
| 消息队列 | NATS JetStream |
| 数据库 | PostgreSQL + TimescaleDB |
| 缓存 | Redis |
| 移动端 | Android: Kotlin + Jetpack Compose / iOS: Swift + SwiftUI |
| 移动BLE | Android: Nordic BLE库 / iOS: CoreBluetooth |
| CI/CD | GitHub Actions |

## 代码结构

```
companion-robot/
├── CLAUDE.md                  — 本文件
├── docs/                      — 架构文档
├── prompts/                   — AI提示词
├── crates/
│   └── bcp-core/              — BCP协议栈 (no_std Rust, 固件和主脑共享)
├── firmware/
│   ├── main/                  — ESP32-S3固件主程序
│   ├── components/            — 固件组件(comm/perception/motion/expression/power/behavior)
│   └── test/                  — 固件单元测试
├── services/
│   ├── gateway/               — 网关服务 (WebSocket/TCP)
│   ├── device-manager/        — 设备管理器
│   ├── scheduler/             — 任务调度器
│   └── ai/                    — AI推理服务
├── mobile/
│   ├── android/               — Android APP
│   └── ios/                   — iOS APP
├── firmware/charging-station/ — 充电站固件
├── tests/
│   ├── protocol/              — BCP协议一致性测试向量
│   ├── integration/           — 集成测试
│   └── load/                  — k6负载测试
├── config/
│   ├── prometheus.yml         — 监控配置
│   └── grafana/               — 仪表盘
├── scripts/
│   └── init.sql               — 数据库初始化
└── docker-compose.yml         — 开发环境
```

## 核心协议 — BCP

BCP (Bundle Command Protocol) 是系统的核心通信协议:
- 二进制帧, 小端序
- 帧头: Magic(0xCB) + Version + TotalLen + SeqNo + CmdCount
- 集束: 单帧可包含最多32条指令
- CRC-16/CCITT校验
- 流控: 滑动窗口(8帧) + 背压
- 重传: 指数退避 200ms→400ms→800ms, 最多3次
- 差量心跳: 无变化时仅3字节
- 免确认: 连续运动指令可设NO_ACK标志

完整规范见 `docs/protocol.md`

## 通信模式

1. **直连模式**: 机器人 ←WebSocket/TCP→ 主脑 (内网, <10ms延迟)
2. **桥接模式**: 机器人 ←BLE→ 手机 ←WebSocket→ 主脑 (外网)
3. **转发模式**: 机器人 ←BLE→ 手机 → 第三方AI API (主脑离线降级)

模式自动切换: 直连断开5s→桥接, 桥接断开10s→降级, 直连恢复→立即切回

## 关键设计决策

- **ESP32-S3**: WiFi+BLE内置, 双核分离通信/感知任务, NPU加速视觉
- **Rust主脑**: 内存安全, tokio高并发, 适合实时通信网关
- **NATS JetStream**: 轻量消息队列, 持久化, 比Kafka更适合IoT场景
- **BCP二进制协议**: 比JSON节省90%带宽, ESP32解析零拷贝
- **能力集注册**: 机器人上报能力, 主脑按能力调度, 支持30cm→120cm平滑演进
- **移动端边缘计算**: 手机分担ASR/NLU/视觉推理, 简单意图本地处理<1s

## 开发约束

### 固件 (ESP32-S3)
- Core 0: 通信任务, Core 1: 感知/执行任务
- 看门狗: 每个关键任务2s内喂狗
- 内存预算: SRAM 512KB, PSRAM 8MB
- 电机PID: 100Hz, IMU: 200Hz, ToF: 50Hz, 环境: 1Hz
- OTA: 双分区, Ed25519签名, 回滚保护

### 主脑 (Rust)
- tokio异步运行时, 每连接一个task
- Gateway: <1ms转发延迟, ~2KB/连接, 1000+并发
- 数据隔离: PostgreSQL RLS + Redis前缀 + NATS Subject
- 熔断器: AI服务50%失败率触发, 30s超时恢复

### 移动端
- BLE MTU: 247字节, 分片传输
- 本地ASR: <500ms, 200+关键词NLU
- 后台保活: 3s空帧心跳
- 资源管控: CPU/内存/电量/温度动态降级

### 性能SLA
- 指令延迟: WiFi <10ms p95, BLE <120ms p95
- 可用性: 99.9% (每月停机<43分钟)
- 语音端到端: 本地~800ms, 主脑~3s

## 安全

- 机器码: eFuse烧录128bit UUID, 不可提取
- 认证: Ed25519挑战-响应 + 证书链
- 通信: TLS 1.3 (WebSocket) / AES-256-GCM (TCP会话密钥)
- BLE: LESC配对
- 权限: Admin/Operator/User/Robot 四级
- 家庭隔离: RLS + Redis前缀 + NATS Subject

## 命令参考

```bash
# 开发环境启动
docker-compose up -d

# 固件编译
cd firmware && idf.py build

# 固件单元测试
idf.py -T components/bcp/test build && idf.py -T components/bcp/test flash monitor

# 主脑编译
cd services && cargo build --release

# 主脑测试
cd services && cargo test --all

# BCP协议测试
cd crates/bcp-core && cargo test

# 负载测试
k6 run tests/load/gateway.js

# 数据库初始化
psql -h localhost -U postgres -f scripts/init.sql
```
