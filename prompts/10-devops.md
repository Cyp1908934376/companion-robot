# DevOps实现提示词

## 角色

你是一个DevOps工程师。请根据以下架构文档实现CI/CD和部署配置。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/main-brain.md` §4 — 部署方案
- `docs/testing-strategy.md` §6 — CI/CD集成
- `docs/performance-sla.md` — 监控指标

## 实现要求

### 1. Docker Compose (开发环境)

```yaml
# docker-compose.yml
version: '3.8'

services:
  gateway:
    build:
      context: .
      dockerfile: services/gateway/Dockerfile
    ports:
      - "8080:8080"
      - "9000:9000"
    environment:
      - NATS_URL=nats://nats:4222
      - REDIS_URL=redis://redis:6379
      - DATABASE_URL=postgres://postgres:postgres@postgres:5432/companion
    depends_on:
      - nats
      - redis
      - postgres
    restart: unless-stopped

  device-manager:
    build:
      context: .
      dockerfile: services/device-manager/Dockerfile
    environment:
      - NATS_URL=nats://nats:4222
      - REDIS_URL=redis://redis:6379
      - DATABASE_URL=postgres://postgres:postgres@postgres:5432/companion
    depends_on:
      - nats
      - redis
      - postgres
    restart: unless-stopped

  scheduler:
    build:
      context: .
      dockerfile: services/scheduler/Dockerfile
    environment:
      - NATS_URL=nats://nats:4222
      - REDIS_URL=redis://redis:6379
      - DATABASE_URL=postgres://postgres:postgres@postgres:5432/companion
    depends_on:
      - nats
      - redis
      - postgres
    restart: unless-stopped

  ai-service:
    build:
      context: .
      dockerfile: services/ai/Dockerfile
    environment:
      - NATS_URL=nats://nats:4222
      - MODEL_PATH=/models
    volumes:
      - ./models:/models
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
    depends_on:
      - nats
    restart: unless-stopped

  nats:
    image: nats:2.10-alpine
    command: "--jetstream --store_dir /data"
    ports:
      - "4222:4222"
      - "8222:8222"
    volumes:
      - nats_data:/data
    restart: unless-stopped

  redis:
    image: redis:7-alpine
    ports:
      - "6379:6379"
    volumes:
      - redis_data:/data
    restart: unless-stopped

  postgres:
    image: timescale/timescaledb:latest-pg16
    environment:
      POSTGRES_DB: companion
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: postgres
    ports:
      - "5432:5432"
    volumes:
      - pg_data:/var/lib/postgresql/data
      - ./scripts/init.sql:/docker-entrypoint-initdb.d/init.sql
    restart: unless-stopped

  prometheus:
    image: prom/prometheus:latest
    ports:
      - "9090:9090"
    volumes:
      - ./config/prometheus.yml:/etc/prometheus/prometheus.yml
    restart: unless-stopped

  grafana:
    image: grafana/grafana:latest
    ports:
      - "3000:3000"
    volumes:
      - grafana_data:/var/lib/grafana
      - ./config/grafana/dashboards:/etc/grafana/provisioning/dashboards
    restart: unless-stopped

volumes:
  nats_data:
  redis_data:
  pg_data:
  grafana_data:
```

### 2. Rust服务Dockerfile

```dockerfile
# services/gateway/Dockerfile
FROM rust:1.77-slim as builder

WORKDIR /app
COPY Cargo.toml Cargo.lock ./
COPY crates/ crates/
COPY services/gateway/ services/gateway/

RUN cargo build --release -p gateway

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/target/release/gateway /usr/local/bin/
EXPOSE 8080 9000
CMD ["gateway"]
```

### 3. Prometheus配置

```yaml
# config/prometheus.yml
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: 'gateway'
    static_configs:
      - targets: ['gateway:9090']

  - job_name: 'device-manager'
    static_configs:
      - targets: ['device-manager:9090']

  - job_name: 'scheduler'
    static_configs:
      - targets: ['scheduler:9090']

  - job_name: 'ai-service'
    static_configs:
      - targets: ['ai-service:9090']

  - job_name: 'nats'
    static_configs:
      - targets: ['nats:8222']
    metrics_path: /varz
```

### 4. Grafana Dashboard

```json
{
  "dashboard": {
    "title": "Companion Robot Cluster",
    "panels": [
      {
        "title": "Online Robots",
        "type": "stat",
        "targets": [{ "expr": "robot_online_count" }]
      },
      {
        "title": "Gateway Connections",
        "type": "gauge",
        "targets": [{ "expr": "gateway_connections" }]
      },
      {
        "title": "Command Latency (p95)",
        "type": "graph",
        "targets": [{ "expr": "histogram_quantile(0.95, gateway_cmd_latency_ms)" }]
      },
      {
        "title": "Task Success Rate",
        "type": "stat",
        "targets": [{ "expr": "rate(task_success_total[5m]) / rate(task_total[5m])" }]
      },
      {
        "title": "Battery Distribution",
        "type": "histogram",
        "targets": [{ "expr": "robot_battery_percent" }]
      }
    ]
  }
}
```

### 5. 数据库初始化

```sql
-- scripts/init.sql
CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE robots (
    id BIGSERIAL PRIMARY KEY,
    family_id BIGINT,
    machine_id BYTEA UNIQUE NOT NULL,
    short_id SMALLINT,
    name VARCHAR(64),
    capabilities JSONB NOT NULL DEFAULT '{}',
    firmware_ver VARCHAR(32),
    status SMALLINT DEFAULT 0,
    last_heartbeat TIMESTAMPTZ,
    battery SMALLINT,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE sensor_data (
    time TIMESTAMPTZ NOT NULL,
    robot_id BIGINT NOT NULL,
    sensor_type SMALLINT NOT NULL,
    data JSONB NOT NULL
);

SELECT create_hypertable('sensor_data', 'time');

CREATE TABLE tasks (
    id BIGSERIAL PRIMARY KEY,
    family_id BIGINT,
    task_type SMALLINT NOT NULL,
    priority SMALLINT DEFAULT 0,
    target_robot SMALLINT,
    status SMALLINT DEFAULT 0,
    params JSONB,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- RLS策略
ALTER TABLE robots ENABLE ROW LEVEL SECURITY;
CREATE POLICY family_isolation ON robots
    USING (family_id = current_setting('app.family_id')::bigint);
```

### 6. 大规模Kubernetes部署

```yaml
# k8s/gateway-deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gateway
spec:
  replicas: 2
  selector:
    matchLabels:
      app: gateway
  template:
    metadata:
      labels:
        app: gateway
    spec:
      containers:
        - name: gateway
          image: companion/gateway:latest
          ports:
            - containerPort: 8080
          resources:
            requests:
              cpu: "500m"
              memory: "256Mi"
            limits:
              cpu: "2000m"
              memory: "1Gi"
          env:
            - name: NATS_URL
              value: "nats://nats:4222"
            - name: REDIS_URL
              valueFrom:
                secretKeyRef:
                  name: companion-secrets
                  key: redis-url
          livenessProbe:
            httpGet:
              path: /health
              port: 8080
            initialDelaySeconds: 10
            periodSeconds: 5
          readinessProbe:
            httpGet:
              path: /ready
              port: 8080
            initialDelaySeconds: 5
            periodSeconds: 3
---
apiVersion: v1
kind: Service
metadata:
  name: gateway
spec:
  selector:
    app: gateway
  ports:
    - port: 8080
      targetPort: 8080
  type: LoadBalancer
```

## 约束

- 开发环境: Docker Compose一键启动
- 生产环境: Kubernetes (200+机器人)
- 数据库: PostgreSQL + TimescaleDB
- 消息队列: NATS JetStream (3节点R3)
- 监控: Prometheus + Grafana
- 日志: 结构化JSON, 输出到stdout
