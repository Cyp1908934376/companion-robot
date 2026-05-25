# 主脑服务架构 — Rust

## 1. 服务拆分

```
┌─────────────────────────────────────────────────────────────┐
│                       主脑集群                                │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Gateway  │  │ Device   │  │ Scheduler│  │ AI       │   │
│  │ 网关服务  │  │ Manager  │  │ 调度器   │  │ 推理服务  │   │
│  │          │  │ 设备管理  │  │          │  │          │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘   │
│       │              │              │              │         │
│  ┌────▼──────────────▼──────────────▼──────────────▼─────┐  │
│  │              NATS JetStream 消息总线                    │  │
│  └────┬──────────────┬──────────────┬──────────────┬─────┘  │
│       │              │              │              │         │
│  ┌────▼─────┐  ┌────▼─────┐  ┌────▼─────┐  ┌────▼─────┐   │
│  │ Redis    │  │ Postgres │  │ MinIO    │  │ 监控     │   │
│  │ 状态缓存  │  │ 持久存储  │  │ 对象存储  │  │ Prometheus│  │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 2. 各服务职责

### 2.1 Gateway (网关服务)

```
职责:
  - WebSocket/TCP 长连接管理 (50+机器人)
  - BCP协议编解码
  - 连接鉴权 (机器码验证)
  - 流量限速
  - 协议转换 (BCP ↔ 内部消息)

技术选型:
  - tokio-tungstenite (异步WebSocket)
  - tokio::net::TcpListener (TCP直连)
  - 每连接一个tokio task, 零拷贝转发

性能目标:
  - 单实例: 1000+并发连接
  - 消息延迟: <1ms (转发到NATS)
  - 内存: ~2KB/连接

关键代码结构:
  gateway/
  ├── src/
  │   ├── main.rs           — 启动入口
  │   ├── config.rs         — 配置
  │   ├── ws_server.rs      — WebSocket服务
  │   ├── tcp_server.rs     — TCP服务
  │   ├── bcp_codec.rs      — BCP编解码 (tokio_util::codec)
  │   ├── auth.rs           — 鉴权
  │   ├── conn_manager.rs   — 连接池管理
  │   └── nats_bridge.rs    — NATS消息桥接
  └── Cargo.toml
```

### 2.2 Device Manager (设备管理)

```
职责:
  - 机器人注册/注销
  - 能力集管理
  - 在线状态维护 (心跳)
  - 设备元数据 CRUD
  - 固件版本管理

数据模型 (PostgreSQL):

  CREATE TABLE robots (
      id          BIGSERIAL PRIMARY KEY,
      machine_id  BYTEA UNIQUE NOT NULL,     -- 128bit 机器码
      short_id    SMALLINT UNIQUE NOT NULL,   -- 集群内短ID
      name        VARCHAR(64),
      capabilities JSONB NOT NULL,
      firmware_ver VARCHAR(32),
      status      SMALLINT DEFAULT 0,        -- 0=离线,1=在线,2=忙碌,3=错误
      last_heartbeat TIMESTAMPTZ,
      battery     SMALLINT,
      rssi        SMALLINT,
      created_at  TIMESTAMPTZ DEFAULT NOW(),
      updated_at  TIMESTAMPTZ DEFAULT NOW()
  );

  CREATE TABLE robot_positions (
      robot_id    BIGINT REFERENCES robots(id),
      x_cm        INT,
      y_cm        INT,
      heading     SMALLINT,                 -- 朝向 0-359°
      updated_at  TIMESTAMPTZ DEFAULT NOW()
  );

  -- TimescaleDB 时序表
  CREATE TABLE sensor_data (
      time        TIMESTAMPTZ NOT NULL,
      robot_id    BIGINT NOT NULL,
      sensor_type SMALLINT NOT NULL,         -- 0=env,1=imu,2=touch
      data        JSONB NOT NULL
  );
  SELECT create_hypertable('sensor_data', 'time');

心跳处理:
  - 收到心跳 → 更新Redis状态 + 写PostgreSQL
  - 心跳超时 (15s) → 标记离线 → 通知调度器
  - 心跳携带的传感器数据 → 写入TimescaleDB
```

### 2.3 Scheduler (调度器)

```
职责:
  - 任务队列管理
  - 多机器人任务分配
  - 优先级调度
  - 任务生命周期管理

任务模型:

  struct Task {
      id: u64,
      task_type: TaskType,       // 巡逻/对话/搬运/充电/编队
      priority: u8,              // 0-255
      target_robot: Option<u16>, // 指定机器人(短ID)或自动分配
      status: TaskStatus,        // Pending/Running/Completed/Failed/Cancelled
      params: Vec<u8>,           // 任务参数 (BCP payload)
      created_at: Instant,
      timeout: Duration,
      retry_count: u8,
  }

调度算法:

  fn assign_task(task: &Task, robots: &[Robot]) -> Option<u16> {
      let candidates: Vec<_> = robots.iter()
          .filter(|r| r.status == Online && r.battery > 20)
          .filter(|r| r.capabilities.satisfies(task.requirements()))
          .collect();

      if candidates.is_empty() {
          return None; // 无可用机器人, 任务挂起
      }

      // 加权评分
      candidates.iter()
          .map(|r| {
              let distance = r.position.distance_to(task.target_pos);
              let load = r.active_tasks as f64;
              let battery = r.battery as f64;
              let affinity = if r.id == task.preferred_robot { 0.0 } else { 1.0 };

              let score = 0.3 * distance.normalize()
                        + 0.2 * load.normalize()
                        + 0.2 * (100.0 - battery).normalize()
                        + 0.3 * affinity;
              (r.short_id, score)
          })
          .min_by(|a, b| a.1.partial_cmp(&b.1).unwrap())
          .map(|(id, _)| id)
  }

任务下发:
  调度器 → NATS("robot.{short_id}.cmd") → Gateway → BCP编码 → 机器人
```

### 2.4 AI 推理服务

```
职责:
  - 语音识别 (ASR)
  - 自然语言理解 (NLU)
  - 对话管理
  - 视觉推理 (人脸/物体/场景)
  - 行为决策

推理链路:

  音频流 → ASR (Whisper) → 文本 → NLU → 对话管理 → 回复
                                              ↓
                                      TTS → 音频 → 机器人播放

  图像帧 → 目标检测 (YOLOv8n) → 场景描述 → 行为决策 → 指令下发

部署方案:
  - GPU节点: 运行Whisper, YOLOv8, LLM推理
  - CPU节点: 对话管理, NLU规则引擎
  - 可选: 调用第三方API (Claude/GPT) 作为备选

模型选择:
  ASR:    whisper-small (244MB, RTF 0.3x on T4)
  视觉:   yolov8n (6MB, 10ms on T4)
  TTS:    piper (CPU即可, <100ms)
  对话:   本地7B模型 或 第三方API
```

## 3. 消息总线设计 (NATS JetStream)

### 3.1 Subject 命名

```
# 机器人 → 主脑
robot.{short_id}.heartbeat     — 心跳
robot.{short_id}.sensor        — 传感器数据
robot.{short_id}.event         — 事件(运动/音频/触觉)
robot.{short_id}.task_status   — 任务进度
robot.{short_id}.response      — 指令响应

# 主脑 → 机器人
robot.{short_id}.cmd           — 控制指令
robot.{short_id}.task          — 任务分配
robot.{short_id}.config        — 配置更新
robot.{short_id}.ota           — OTA数据

# 广播
broadcast.all.cmd              — 全体指令
broadcast.emergency            — 紧急广播
cluster.form                   — 编队指令

# 服务间
internal.scheduler.assign      — 调度器分配事件
internal.device.status         — 设备状态变更
internal.ai.result             — AI推理结果
```

### 3.2 JetStream 配置

```yaml
# 关键流配置
streams:
  ROBOT_EVENTS:
    subjects: ["robot.*.event", "robot.*.sensor"]
    retention: limits
    max_age: 1h          # 传感器数据保留1小时
    storage: file
    replicas: 1

  COMMANDS:
    subjects: ["robot.*.cmd", "robot.*.task"]
    retention: workqueue  # 工作队列模式, 消费后删除
    max_age: 30s          # 指令30秒过期
    storage: memory       # 内存存储, 低延迟

  AI_TASKS:
    subjects: ["internal.ai.*"]
    retention: workqueue
    max_age: 5m
    storage: file
    replicas: 2
```

## 4. 水平扩展方案

### 4.1 小规模 (50-200台机器人)

```yaml
# docker-compose.yml
services:
  gateway:
    image: companion/gateway
    replicas: 2           # 2个网关实例
    ports:
      - "8080:8080"       # WebSocket
      - "9000:9000"       # TCP

  device-manager:
    image: companion/device-manager
    replicas: 1

  scheduler:
    image: companion/scheduler
    replicas: 1

  ai-service:
    image: companion/ai-service
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1    # GPU

  nats:
    image: nats:latest
    command: "--jetstream"

  redis:
    image: redis:7-alpine

  postgres:
    image: timescale/timescaledb:latest-pg16
```

### 4.2 大规模 (200+ 台机器人)

```
扩展策略:
  Gateway:     无状态, 水平扩展, 前置LB (Nginx/HAProxy)
               每实例 1000连接, 500台需1-2实例
  Device Mgr:  主备热切换, 读写分离
  Scheduler:   单主 (避免脑裂), 热备
  AI Service:  GPU节点池, 按需扩缩
  NATS:        3节点集群, JetStream R3复制
  Postgres:    主从复制, TimescaleDB分布式超表

服务发现:
  - Consul / etcd 注册中心
  - 或 NATS 自带的服务发现

负载均衡:
  - WebSocket: 基于机器码的会话粘滞
  - 避免同一机器人被分到不同Gateway实例
```

## 5. API 接口

### 5.1 REST API (管理面)

```
POST   /api/v1/robots/register     — 注册新机器人
GET    /api/v1/robots               — 列出所有机器人
GET    /api/v1/robots/{id}          — 机器人详情
PUT    /api/v1/robots/{id}/config   — 更新配置
DELETE /api/v1/robots/{id}          — 注销机器人

POST   /api/v1/tasks                — 创建任务
GET    /api/v1/tasks                — 任务列表
GET    /api/v1/tasks/{id}           — 任务详情
DELETE /api/v1/tasks/{id}           — 取消任务

POST   /api/v1/swarm/form           — 创建编队
POST   /api/v1/swarm/dissolve       — 解散编队

GET    /api/v1/metrics              — 集群指标
GET    /api/v1/events               — 事件流 (SSE)
```

### 5.2 WebSocket API (数据面)

```
ws://gateway:8080/ws/{machine_id}

鉴权: 首帧发送 BCP REGISTER 指令
连接后: 双向 BCP 帧流
```

## 6. 监控与告警

```
指标 (Prometheus):
  - robot_online_count          — 在线机器人数
  - robot_battery_distribution  — 电量分布
  - cmd_latency_ms              — 指令延迟
  - task_queue_depth            — 任务队列深度
  - task_success_rate           — 任务成功率
  - gateway_connections         — 网关连接数
  - nats_msg_rate               — NATS消息速率

告警规则:
  - 机器人离线 > 5分钟
  - 电量 < 15%
  - 指令延迟 > 500ms
  - 任务失败率 > 10%
  - Gateway连接数 > 80%容量

Grafana Dashboard:
  - 集群总览 (在线/离线/电量/负载)
  - 单机器人详情 (传感器实时数据, 指令历史)
  - 任务执行统计
```
