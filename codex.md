# Companion Robot — Codex 项目指令

你正在协助开发一个陪伴机器人集群系统。请先阅读 `docs/` 目录下的所有架构文档，理解完整系统设计后再开始编写代码。

## 你需要知道的

### 系统组成
- **机器人端**: ESP32-S3 (双核240MHz, WiFi+BLE), 运行ESP-IDF/FreeRTOS
- **主脑服务**: Rust (tokio异步), 包含网关/设备管理/调度器/AI推理
- **移动APP**: Android(Kotlin)/iOS(Swift), 桥接+边缘计算
- **充电站**: ESP32-C3, 红外信标引导

### 通信协议 — BCP
所有组件通过BCP (Bundle Command Protocol) 通信:
- 二进制帧, 小端序, Magic=0xCB, 最大1024字节
- 集束: 单帧最多32条指令
- CRC-16/CCITT校验
- 完整规范见 `docs/protocol.md`

### 代码组织
```
crates/bcp-core/        — 协议栈 (no_std Rust, 跨平台共享)
firmware/               — ESP32-S3固件 (C, ESP-IDF)
services/gateway/       — 网关 (Rust)
services/device-manager/ — 设备管理 (Rust)
services/scheduler/     — 调度器 (Rust)
mobile/android/         — Android APP
mobile/ios/             — iOS APP
```

## 编码规范

### Rust
- Edition 2021, 使用 `anyhow` 做错误处理
- 异步运行时: tokio
- BCP编解码: 使用 `bytes` crate
- 序列化: `serde` + `serde_json`
- 测试: `#[cfg(test)]` 模块内联测试

### C (ESP-IDF)
- ESP-IDF v5.x 风格
- 日志: ESP_LOGI/ESP_LOGW/ESP_LOGE
- 任务: xTaskCreatePinnedToCore, Core 0=通信, Core 1=感知
- 错误处理: ESP_ERROR_CHECK
- 内存: 注意SRAM限制, 大缓冲用PSRAM

### Kotlin (Android)
- Kotlin 1.9+, Jetpack Compose
- BLE: Nordic BLE库
- 协程: Flow处理异步数据流
- 架构: MVVM

### Swift (iOS)
- Swift 5.9+, SwiftUI
- BLE: CoreBluetooth
- 并发: async/await + AsyncSequence

## 关键约束

1. **BCP协议是共享的**: `crates/bcp-core` 被固件和主脑同时使用, 修改必须考虑no_std兼容
2. **固件内存有限**: SRAM 512KB, PSRAM 8MB, 注意内存分配
3. **实时性要求**: 电机控制100Hz, IMU 200Hz, 不能阻塞
4. **多平台一致性**: BCP编解码在Rust/C/Kotlin/Swift中必须行为一致
5. **家庭数据隔离**: 所有数据库查询必须带family_id过滤
6. **错误降级**: 传感器故障必须有降级策略, 不能直接panic

## 任务优先级

当不知道做什么时, 按以下顺序:
1. 先实现 `crates/bcp-core` — 协议栈是所有组件的基础
2. 再实现主脑网关 — 能接收连接
3. 再实现固件通信层 — 能连接主脑
4. 再实现感知和执行 — 传感器和电机
5. 最后实现移动端 — 桥接和UI

## 文档索引

| 文档 | 内容 |
|------|------|
| `docs/architecture.md` | 总体架构、拓扑、通信模式 |
| `docs/protocol.md` | BCP协议完整规范 |
| `docs/robot-firmware.md` | 固件架构、FreeRTOS任务、引脚定义 |
| `docs/main-brain.md` | 主脑服务拆分、数据库Schema |
| `docs/security.md` | 认证、加密、权限 |
| `docs/hardware-form-factor.md` | 硬件BOM、充电站设计 |
| `docs/mobile-sdk.md` | 移动端SDK、边缘计算 |
| `docs/error-handling.md` | 错误码、降级策略 |
| `docs/performance-sla.md` | SLA、容量规划 |
| `docs/multi-user.md` | 多用户、家庭共享 |
| `docs/testing-strategy.md` | 测试策略、CI/CD |

## 示例: 添加一条新的BCP指令

1. 在 `docs/protocol.md` 的指令集表格中添加新指令ID和Payload定义
2. 在 `crates/bcp-core/src/command.rs` 的 `Command` 枚举中添加变体
3. 在 `crates/bcp-core/src/codec.rs` 的 `encode`/`decode` 中添加处理
4. 在 `crates/bcp-core/src/lib.rs` 添加测试用例
5. 在固件 `firmware/components/bcp/` 中添加C绑定
6. 在主脑 `services/gateway/` 中添加NATS转发逻辑
7. 在移动端添加对应的BLE收发处理

## 运行测试

```bash
# 协议栈测试
cd crates/bcp-core && cargo test

# 主脑测试 (需要Docker)
docker-compose up -d nats redis postgres
cd services && cargo test --all

# 固件测试 (需要ESP32开发板)
cd firmware && idf.py -T components/bcp/test build flash monitor

# 负载测试
docker-compose up -d
k6 run tests/load/gateway.js
```
