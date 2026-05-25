# 主脑设备管理器实现提示词

## 角色

你是一个Rust后端工程师。请根据以下架构文档实现主脑的设备管理器服务。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/main-brain.md` §2.2 — 设备管理职责
- `docs/security.md` — 认证与加密
- `docs/multi-user.md` — 多用户数据模型

## 项目结构

```
services/device-manager/
├── Cargo.toml
├── src/
│   ├── main.rs
│   ├── config.rs
│   ├── registry.rs          — 设备注册/注销
│   ├── heartbeat.rs         — 心跳处理
│   ├── capability.rs        — 能力集管理
│   ├── nats_handler.rs      — NATS消息处理
│   ├── db.rs                — 数据库操作
│   ├── redis.rs             — Redis状态缓存
│   ├── metrics.rs
│   └── error.rs
```

## 关键实现

### 1. 设备注册

```rust
pub async fn handle_register(
    machine_id: MachineId,
    caps: Capabilities,
    firmware_ver: [u8; 4],
    db: &PgPool,
    redis: &RedisClient,
) -> Result<u16> {
    // 查询或创建设备记录
    let robot = sqlx::query_as!(
        Robot,
        "SELECT * FROM robots WHERE machine_id = $1",
        machine_id.as_bytes()
    )
    .fetch_optional(db)
    .await?;

    let short_id = match robot {
        Some(r) => {
            // 已注册, 更新信息
            sqlx::query!(
                "UPDATE robots SET capabilities = $1, firmware_ver = $2, status = 1, last_heartbeat = NOW() WHERE machine_id = $3",
                serde_json::to_value(&caps)?,
                format_version(firmware_ver),
                machine_id.as_bytes()
            )
            .execute(db)
            .await?;
            r.short_id as u16
        }
        None => {
            // 新注册, 分配短ID
            let short_id = allocate_short_id(db).await?;
            sqlx::query!(
                "INSERT INTO robots (machine_id, short_id, capabilities, firmware_ver, status) VALUES ($1, $2, $3, $4, 1)",
                machine_id.as_bytes(),
                short_id as i16,
                serde_json::to_value(&caps)?,
                format_version(firmware_ver)
            )
            .execute(db)
            .await?;
            short_id
        }
    };

    // 更新Redis状态
    redis.set_robot_online(short_id, &caps).await?;

    // 发布设备上线事件
    nats.publish("internal.device.status", serde_json::json!({
        "short_id": short_id,
        "event": "online",
        "capabilities": caps
    }).to_string().into()).await?;

    Ok(short_id)
}
```

### 2. 心跳处理

```rust
pub async fn handle_heartbeat(
    short_id: u16,
    status: u8,
    battery: u8,
    rssi: i8,
    task_id: u16,
    db: &PgPool,
    redis: &RedisClient,
) -> Result<()> {
    // 更新Redis (实时状态)
    redis.update_heartbeat(short_id, status, battery, rssi).await?;

    // 更新PostgreSQL (持久化)
    sqlx::query!(
        "UPDATE robots SET status = $1, battery = $2, last_heartbeat = NOW() WHERE short_id = $3",
        status as i16,
        battery as i16,
        short_id as i16
    )
    .execute(db)
    .await?;

    // 检查低电量
    if battery < 15 {
        nats.publish("internal.device.alert", serde_json::json!({
            "short_id": short_id,
            "alert": "low_battery",
            "battery": battery
        }).to_string().into()).await?;
    }

    Ok(())
}
```

### 3. 离线检测

```rust
pub async fn run_offline_detector(db: &PgPool, redis: &RedisClient, nats: &NatsClient) -> Result<()> {
    let mut interval = tokio::time::interval(Duration::from_secs(5));

    loop {
        interval.tick().await;

        // 查找超时设备 (15s无心跳)
        let offline_robots = sqlx::query!(
            "SELECT short_id FROM robots WHERE status = 1 AND last_heartbeat < NOW() - INTERVAL '15 seconds'"
        )
        .fetch_all(db)
        .await?;

        for robot in offline_robots {
            let short_id = robot.short_id as u16;

            // 标记离线
            sqlx::query!("UPDATE robots SET status = 0 WHERE short_id = $1", robot.short_id)
                .execute(db)
                .await?;

            redis.set_robot_offline(short_id).await?;

            // 发布离线事件
            nats.publish("internal.device.status", serde_json::json!({
                "short_id": short_id,
                "event": "offline"
            }).to_string().into()).await?;

            // 通知调度器
            nats.publish("internal.scheduler.assign", serde_json::json!({
                "event": "robot_offline",
                "short_id": short_id
            }).to_string().into()).await?;
        }
    }
}
```

## 数据库Schema

```sql
-- 参考 docs/multi-user.md §1.2 的完整Schema
-- robots表需要family_id字段实现多租户隔离
-- 使用PostgreSQL RLS策略强制数据隔离
```

## 约束

- 心跳处理延迟: <5ms
- Redis + PostgreSQL双写 (Redis用于实时, PG用于持久化)
- 离线检测: 15s超时
- 家庭数据隔离
