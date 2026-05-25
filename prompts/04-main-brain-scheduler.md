# 主脑调度器实现提示词

## 角色

你是一个Rust后端工程师。请根据以下架构文档实现主脑的任务调度器服务。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/main-brain.md` §2.3 — 调度器职责与算法
- `docs/performance-sla.md` — 延迟SLA
- `docs/error-handling.md` — 任务错误码

## 项目结构

```
services/scheduler/
├── Cargo.toml
├── src/
│   ├── main.rs
│   ├── config.rs
│   ├── scheduler.rs         — 调度核心逻辑
│   ├── task_queue.rs        — 优先级任务队列
│   ├── assignment.rs        — 机器人分配算法
│   ├── nats_handler.rs      — NATS消息处理
│   ├── metrics.rs
│   └── error.rs
```

## 关键实现

### 1. 任务模型

```rust
pub struct Task {
    pub id: u64,
    pub task_type: TaskType,
    pub priority: u8,
    pub target_robot: Option<u16>,  // 指定机器人或自动分配
    pub status: TaskStatus,
    pub params: Vec<u8>,
    pub created_at: Instant,
    pub timeout: Duration,
    pub retry_count: u8,
    pub family_id: i64,
}

pub enum TaskType {
    Patrol { waypoints: Vec<(i16, i16)> },
    FollowUser { user_id: i64 },
    Charge,
    Dialogue { context_id: u64 },
    Custom { type_id: u8 },
}

pub enum TaskStatus {
    Pending,
    Assigned(u16),      // 分配给哪个机器人
    Running(u16),
    Completed,
    Failed(TaskError),
    Cancelled,
}
```

### 2. 分配算法

```rust
pub fn assign_task(task: &Task, robots: &[Robot]) -> Option<u16> {
    let candidates: Vec<_> = robots.iter()
        .filter(|r| r.status == RobotStatus::Online)
        .filter(|r| r.battery > 20)
        .filter(|r| r.capabilities.satisfies(task.requirements()))
        .filter(|r| task.family_id == r.family_id)  // 家庭隔离
        .collect();

    if candidates.is_empty() {
        return None;
    }

    candidates.iter()
        .map(|r| {
            let distance = r.position.distance_to(task.target_pos());
            let load = r.active_tasks as f64;
            let battery = r.battery as f64;
            let affinity = if r.id == task.preferred_robot() { 0.0 } else { 1.0 };

            let score = 0.3 * normalize(distance, 0.0, 1000.0)
                      + 0.2 * normalize(load, 0.0, 5.0)
                      + 0.2 * normalize(100.0 - battery, 0.0, 100.0)
                      + 0.3 * affinity;
            (r.short_id, score)
        })
        .min_by(|a, b| a.1.partial_cmp(&b.1).unwrap())
        .map(|(id, _)| id)
}
```

### 3. 调度循环

```rust
pub async fn run_scheduler(config: Config) -> Result<()> {
    let nats = NatsClient::connect(&config.nats_url).await?;
    let db = PgPool::connect(&config.database_url).await?;
    let redis = RedisClient::connect(&config.redis_url).await?;

    // 订阅任务创建事件
    let mut task_sub = nats.subscribe("internal.scheduler.assign").await?;

    // 订阅机器人状态变更
    let mut status_sub = nats.subscribe("robot.*.task_status").await?;

    loop {
        tokio::select! {
            // 新任务
            Some(msg) = task_sub.next() => {
                let task: Task = serde_json::from_slice(&msg.payload)?;
                let robots = load_robots(&db, &redis).await?;

                if let Some(robot_id) = assign_task(&task, &robots) {
                    // 下发任务
                    let cmd = Command::TaskAssign {
                        task_type: task.type_id(),
                        priority: task.priority,
                        params: task.params.clone(),
                    };
                    nats.publish(
                        &format!("robot.{}.task", robot_id),
                        encode_cmd(&cmd)?,
                    ).await?;

                    // 更新状态
                    update_task_status(&db, task.id, TaskStatus::Assigned(robot_id)).await?;
                } else {
                    // 无可用机器人, 挂起
                    update_task_status(&db, task.id, TaskStatus::Pending).await?;
                }
            }

            // 任务状态更新
            Some(msg) = status_sub.next() => {
                let status: TaskStatusMsg = serde_json::from_slice(&msg.payload)?;
                update_task_status(&db, status.task_id, status.status).await?;

                // 任务完成, 检查是否有挂起任务
                if status.status == TaskStatus::Completed {
                    check_pending_tasks(&db, &nats).await?;
                }
            }
        }
    }
}
```

## 约束

- 单主模式 (避免脑裂)
- 任务分配延迟: <100ms (p95)
- 支持50+机器人并发
- 家庭数据隔离
- NATS JetStream持久化
