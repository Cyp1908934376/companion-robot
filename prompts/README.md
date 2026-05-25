# 实现提示词

本目录包含基于架构文档生成的AI辅助实现提示词。
每个提示词对应一个可独立开发的模块，包含完整的技术上下文和实现要求。

## 使用方式

将对应提示词复制给AI助手（如Claude），它将根据架构文档生成符合规范的代码。

## 提示词清单

| 文件 | 模块 | 语言 | 说明 |
|------|------|------|------|
| `01-bcp-protocol.md` | BCP协议栈 | Rust | 三个平台共享的协议实现 |
| `02-robot-firmware.md` | 机器人固件 | C (ESP-IDF) | ESP32-S3完整固件 |
| `03-main-brain-gateway.md` | 网关服务 | Rust | WebSocket/TCP网关 |
| `04-main-brain-scheduler.md` | 调度器 | Rust | 任务调度与分配 |
| `05-main-brain-device-mgr.md` | 设备管理 | Rust | 机器人注册与状态管理 |
| `06-mobile-ble.md` | 移动端BLE | Kotlin/Swift | BLE通信层 |
| `07-mobile-bridge.md` | 移动端桥接 | Kotlin/Swift | 双通道转发+本地AI |
| `08-charging-station.md` | 充电站固件 | C | 红外信标+充电管理 |
| `09-testing.md` | 测试套件 | 多语言 | 单元测试+集成测试 |
| `10-devops.md` | DevOps | YAML/Shell | CI/CD+部署 |
