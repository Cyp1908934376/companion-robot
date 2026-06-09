-- Companion Robot — Database initialization
-- Target: PostgreSQL 16 + TimescaleDB

-- ── Extensions ────────────────────────────────────────────────
CREATE EXTENSION IF NOT EXISTS timescaledb;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- ── Robots table ──────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS robots (
    id              BIGSERIAL PRIMARY KEY,
    machine_id      BYTEA UNIQUE NOT NULL,
    short_id        SMALLINT UNIQUE NOT NULL,
    name            VARCHAR(64) NOT NULL,
    capabilities    JSONB NOT NULL DEFAULT '{}',
    firmware_ver    VARCHAR(32) NOT NULL DEFAULT '0.0.0',
    status          SMALLINT NOT NULL DEFAULT 0,       -- 0=offline 1=online 2=busy 3=error
    last_heartbeat  TIMESTAMPTZ,
    battery         SMALLINT,
    rssi            SMALLINT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ── Robot positions ───────────────────────────────────────────
CREATE TABLE IF NOT EXISTS robot_positions (
    robot_id    BIGINT REFERENCES robots(id) ON DELETE CASCADE,
    x_cm        INT NOT NULL DEFAULT 0,
    y_cm        INT NOT NULL DEFAULT 0,
    heading     SMALLINT NOT NULL DEFAULT 0,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (robot_id)
);

-- ── Sensor data (TimescaleDB hypertable) ──────────────────────
CREATE TABLE IF NOT EXISTS sensor_data (
    time        TIMESTAMPTZ NOT NULL,
    robot_id    BIGINT NOT NULL,
    sensor_type SMALLINT NOT NULL,   -- 0=env 1=imu 2=touch 3=audio 4=vision
    data        JSONB NOT NULL
);
SELECT create_hypertable('sensor_data', 'time', if_not_exists => TRUE);

-- Index for per-robot queries
CREATE INDEX IF NOT EXISTS idx_sensor_data_robot_time
    ON sensor_data (robot_id, time DESC);

-- ── Tasks table ───────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS tasks (
    id                      BIGSERIAL PRIMARY KEY,
    task_type               VARCHAR(32) NOT NULL,
    priority                SMALLINT NOT NULL DEFAULT 128,
    target_robot_short_id   SMALLINT,
    preferred_robot_short_id SMALLINT,
    status                  SMALLINT NOT NULL DEFAULT 0,  -- 0=pending 1=running 2=completed 3=failed 4=cancelled
    params                  JSONB NOT NULL DEFAULT '{}',
    target_x_cm             INT,
    target_y_cm             INT,
    created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    started_at              TIMESTAMPTZ,
    completed_at            TIMESTAMPTZ,
    timeout_secs            INT NOT NULL DEFAULT 30,
    retry_count             SMALLINT NOT NULL DEFAULT 0,
    max_retries             SMALLINT NOT NULL DEFAULT 3
);

CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_robot ON tasks(target_robot_short_id);
CREATE INDEX IF NOT EXISTS idx_tasks_priority ON tasks(priority, created_at);

-- ── Event log ─────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS event_log (
    id          BIGSERIAL PRIMARY KEY,
    time        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    robot_id    BIGINT REFERENCES robots(id) ON DELETE SET NULL,
    event_type  VARCHAR(32) NOT NULL,
    severity    SMALLINT NOT NULL DEFAULT 0,  -- 0=info 1=warning 2=error
    data        JSONB NOT NULL DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS idx_event_log_time ON event_log(time DESC);
CREATE INDEX IF NOT EXISTS idx_event_log_robot ON event_log(robot_id, time DESC);

-- ── Session / dialogue history ────────────────────────────────
CREATE TABLE IF NOT EXISTS dialogue_sessions (
    id          BIGSERIAL PRIMARY KEY,
    session_id  VARCHAR(64) UNIQUE NOT NULL,
    robot_id    BIGINT REFERENCES robots(id) ON DELETE SET NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS dialogue_messages (
    id          BIGSERIAL PRIMARY KEY,
    session_id  VARCHAR(64) REFERENCES dialogue_sessions(session_id) ON DELETE CASCADE,
    role        VARCHAR(16) NOT NULL,  -- "user" or "assistant"
    content     TEXT NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_dialogue_messages_session
    ON dialogue_messages(session_id, created_at);

-- ── Trigger: auto-update updated_at ───────────────────────────
CREATE OR REPLACE FUNCTION update_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_robots_updated
    BEFORE UPDATE ON robots
    FOR EACH ROW EXECUTE FUNCTION update_timestamp();
