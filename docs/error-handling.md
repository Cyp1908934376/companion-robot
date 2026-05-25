# 错误处理与恢复

## 1. 错误码体系

### 1.1 系统级错误码 (0xExxx)

```
BCP ERROR 指令 (0x00FF):
  Payload: [错误码(2B), 错误级别(1B), 关联SeqNo(2B), 附加数据(NB)]

错误级别:
  0x01 = WARNING  (警告, 不影响运行)
  0x02 = ERROR    (错误, 功能降级)
  0x03 = FATAL    (致命, 需要干预)

错误码表:

  协议层 (0x01xx):
    0x0101 = E_PROTO_BAD_MAGIC        Magic校验失败
    0x0102 = E_PROTO_BAD_VERSION      版本不支持
    0x0103 = E_PROTO_BAD_CRC          CRC校验失败
    0x0104 = E_PROTO_TOO_LONG         帧长度超限
    0x0105 = E_PROTO_BAD_CMD          未知指令ID
    0x0106 = E_PROTO_BAD_PAYLOAD      Payload格式错误
    0x0107 = E_PROTO_OVERFLOW         接收缓冲区溢出
    0x0108 = E_PROTO_SEQ_MISMATCH     序列号不匹配
    0x0109 = E_PROTO_RATE_LIMIT       速率限制

  连接层 (0x02xx):
    0x0201 = E_CONN_TIMEOUT           连接超时
    0x0202 = E_CONN_AUTH_FAIL         认证失败
    0x0203 = E_CONN_REJECTED          连接被拒 (黑名单/容量满)
    0x0204 = E_CONN_LOST              连接丢失
    0x0205 = E_CONN_BLE_MTU_FAIL      MTU协商失败
    0x0206 = E_CONN_WS_HANDSHAKE      WebSocket握手失败

  传感器层 (0x03xx):
    0x0301 = E_SENSOR_I2C_TIMEOUT     I2C总线超时
    0x0302 = E_SENSOR_I2C_NACK        I2C无应答
    0x0303 = E_SENSOR_SPI_DMA         SPI DMA错误
    0x0304 = E_SENSOR_CAMERA_INIT     摄像头初始化失败
    0x0305 = E_SENSOR_CAMERA_FRAME    摄像头帧采集失败
    0x0306 = E_SENSOR_MIC_OVERFLOW    麦克风缓冲区溢出
    0x0307 = E_SENSOR_IMU_DATA_RDY    IMU数据就绪超时
    0x0308 = E_SENSOR_TOF_RANGE       ToF超出量程
    0x0309 = E_SENSOR_BATTERY_LOW     电池电压过低

  执行层 (0x04xx):
    0x0401 = E_MOTOR_STALL            电机堵转
    0x0402 = E_MOTOR_OVERCURRENT      电机过流
    0x0403 = E_SERVO_RANGE            舵机角度超限
    0x0404 = E_SERVO_TIMEOUT          舵机响应超时
    0x0405 = E_CHARGE_DOCK_FAIL       充电对接失败
    0x0406 = E_CHARGE_TIMEOUT         充电超时

  系统层 (0x05xx):
    0x0501 = E_SYS_HEAP_OOM           内存不足
    0x0502 = E_SYS_TASK_WATCHDOG      任务看门狗超时
    0x0503 = E_SYS_STACK_OVERFLOW     栈溢出
    0x0504 = E_SYS_PANIC              系统崩溃
    0x0505 = E_SYS_OTA_VERIFY         OTA校验失败
    0x0506 = E_SYS_OTA_ROLLBACK       OTA回滚
    0x0507 = E_SYS_NVS_CORRUPT        NVS数据损坏

  任务层 (0x06xx):
    0x0601 = E_TASK_UNKNOWN           未知任务类型
    0x0602 = E_TASK_BUSY              机器人忙碌
    0x0603 = E_TASK_TIMEOUT           任务超时
    0x0604 = E_TASK_CANCELLED         任务被取消
    0x0605 = E_TASK_NO_ROBOT          无可用机器人
    0x0606 = E_TASK_CAPABILITY        能力不匹配
```

## 2. 传感器故障降级

### 2.1 故障检测

```c
// 每个传感器任务内置健康检查
void sensor_health_check() {
    // I2C设备: 定期读取WHO_AM_I寄存器
    if (i2c_read(dev, WHO_AM_I_REG) != expected_id) {
        sensor_fault_count[SENSOR_IMU]++;
        if (sensor_fault_count[SENSOR_IMU] > 3) {
            report_error(E_SENSOR_I2C_NACK, SENSOR_IMU);
            sensor_status[SENSOR_IMU] = FAULT;
        }
    } else {
        sensor_fault_count[SENSOR_IMU] = 0;
        sensor_status[SENSOR_IMU] = OK;
    }

    // 摄像头: 检查帧采集是否超时
    if (frame_acquire_timeout > 500ms) {
        report_error(E_SENSOR_CAMERA_FRAME);
        sensor_status[SENSOR_CAMERA] = FAULT;
    }

    // ToF: 检查返回值是否有效
    if (tof_distance == 0xFFFF || tof_distance == 0) {
        report_error(E_SENSOR_TOF_RANGE);
        sensor_status[SENSOR_TOF] = DEGRADED;
    }
}
```

### 2.2 降级策略

```
┌──────────────┬─────────────────────────────────────────────────┐
│ 传感器       │ 故障后行为                                      │
├──────────────┼─────────────────────────────────────────────────┤
│ 摄像头       │ 关闭视觉任务, 继续其他功能                      │
│              │ 上报 FAULT, 主脑停止请求图像                    │
│              │ 尝试重启: power_cycle → 重新初始化              │
├──────────────┼─────────────────────────────────────────────────┤
│ 麦克风       │ 关闭VAD/ASR, 仅保留按键触发对话                 │
│              │ 上报 FAULT, 表情显示"听不见"                    │
│              │ 尝试重启I2S驱动                                 │
├──────────────┼─────────────────────────────────────────────────┤
│ IMU          │ 降级为纯编码器定位, 姿态精度下降                 │
│              │ 上报 DEGRADED, 禁用跌倒检测                     │
│              │ 尝试重新初始化BMI270                            │
├──────────────┼─────────────────────────────────────────────────┤
│ 单个ToF      │ 关闭该方向避障, 其他方向继续                     │
│              │ 上报 DEGRADED, 运动速度限制为50%                │
│              │ 尝试I2C地址重置                                 │
├──────────────┼─────────────────────────────────────────────────┤
│ 全部ToF      │ 停止自主运动, 仅允许远程控制 (速度30%)          │
│              │ 上报 FAULT, 表情显示"请小心"                    │
├──────────────┼─────────────────────────────────────────────────┤
│ 环境传感器   │ 停止环境上报, 其他功能不受影响                   │
│              │ 上报 DEGRADED                                   │
├──────────────┼─────────────────────────────────────────────────┤
│ 触觉         │ 停止触摸交互, 其他功能不受影响                   │
│              │ 上报 DEGRADED                                   │
└──────────────┴─────────────────────────────────────────────────┘

I2C总线恢复:
  if (i2c_bus_hung) {
      i2c_bus_reset();          // 发送9个时钟脉冲
      vTaskDelay(10ms);
      i2c_bus_scan();           // 重新扫描设备
      reinit_failed_devices();  // 重新初始化故障设备
  }
```

## 3. 主脑服务故障处理

### 3.1 熔断器 (Circuit Breaker)

```
状态机:
  ┌─────────┐  失败率>50%  ┌─────────┐  超时30s  ┌─────────┐
  │ CLOSED  │ ──────────► │  OPEN   │ ─────────► │HALF_OPEN│
  │ 正常    │ ◄────────── │  熔断   │ ◄───────── │ 半开    │
  └─────────┘  成功率>80%  └─────────┘  继续失败  └─────────┘
                                                  │ 成功
                                                  ▼
                                              CLOSED

应用于:
  - Gateway → AI推理服务 (故障时降级为本地处理)
  - Gateway → DeviceManager (故障时使用Redis缓存)
  - Scheduler → AI服务 (故障时跳过能力匹配)

配置:
  failure_threshold:  50% (10次请求中5次失败)
  success_threshold:  80% (半开状态下5次中4次成功)
  timeout:            30s (熔断后等待时间)
  half_open_requests: 5 (半开状态试探请求数)
```

### 3.2 网络分区处理

```
场景1: Gateway ↔ Scheduler 断开
  影响: 新任务无法分配
  处理:
    - Gateway缓存任务请求 (本地队列, 最多100条)
    - 心跳继续转发 (直接写Redis)
    - 恢复后批量提交缓存任务
    - 超时5min的任务自动取消

场景2: Gateway ↔ AI服务 断开
  影响: 语音/视觉推理不可用
  处理:
    - 熔断器触发, 请求不再发送
    - 语音: 降级为移动端本地处理
    - 视觉: 降级为仅运动检测
    - 恢复后熔断器半开探测

场景3: NATS集群分区
  影响: 消息丢失或重复
  处关:
    - JetStream持久化保证消息不丢
    - 消费者幂等处理 (基于SeqNo去重)
    - 分区恢复后自动重平衡

场景4: 移动端 ↔ 主脑 断开 (但BLE连机器人)
  影响: 主脑无法控制机器人
  处理:
    - 移动端切换为本地处理模式
    - 本地行为引擎接管
    - 缓存任务指令, 恢复后同步
```

### 3.3 数据一致性

```
Redis ↔ PostgreSQL 不一致:
  检测: 定时任务每5min对比Redis设备状态和PostgreSQL
  修复: 以PostgreSQL为准, 刷新Redis
  预防: 写操作先写PostgreSQL, 成功后更新Redis

TimescaleDB数据间隙:
  检测: 监控传感器数据到达率
  修复: 间隙>30s标记为MISSING, 不做插值
  预防: 心跳携带传感器摘要, 间隙可追溯

BCP帧数据损坏 (通过CRC但内容无效):
  检测: Payload长度与CmdID期望不匹配
  处理: 返回E_PROTO_BAD_PAYLOAD, 丢弃该帧
  预防: 关键指令(注册/OTA)使用额外校验和
```

## 4. 级联故障防护

### 4.1 背压传播

```
AI推理服务过载:
  AI服务 → 发布NATS消息"overloaded"
  Gateway → 收到后降低发送频率
  移动端 → 收到后增加本地处理比例

传感器数据洪泛:
  机器人 → 检测到数据积压 (队列>80%)
  机器人 → 降低采样率 (10Hz→2Hz)
  机器人 → 降低图像分辨率 (VGA→QVGA)
  恢复: 队列<30% → 恢复原始采样率
```

### 4.2 超时预算

```
端到端超时: 5s (用户可感知延迟上限)

超时分解:
  机器人→Gateway:    500ms (BLE) / 50ms (WiFi)
  Gateway→NATS:      10ms
  NATS→Scheduler:    50ms
  Scheduler决策:     100ms
  NATS→AI服务:       50ms
  AI推理:            2s (ASR) / 500ms (视觉)
  返回路径:          同上

  总计: ~3s (WiFi直连) / ~4s (BLE桥接)

超时处理:
  任一环节超时 → 返回E_CONN_TIMEOUT
  超时3次 → 降级为本地处理
```

### 4.3 速率限制

```
Gateway级别:
  - 每连接: 100 msg/s (正常), 500 msg/s (突发)
  - 全局: 10000 msg/s
  - 超限: 返回E_PROTO_RATE_LIMIT, 丢弃消息

API级别:
  - REST API: 100 req/s per user
  - WebSocket: 无额外限制 (受Gateway限制)
  - 超限: HTTP 429 Too Many Requests

机器人级别:
  - 上行: 50 msg/s (传感器数据)
  - 下行: 100 msg/s (控制指令)
  - 超限: 队列缓冲, 溢出丢弃低优先级
```

## 5. 恢复流程

### 5.1 机器人崩溃恢复

```
看门狗:
  - 任务看门狗: 每个关键任务必须2s内喂狗
  - 看门狗超时 → 记录崩溃信息到NVS → 重启
  - 连续崩溃3次 → 进入安全模式 (仅BLE, 低速)

崩溃转储:
  - ESP-IDF Core Dump → Flash保留区
  - 重启后通过OTA上传到主脑
  - 包含: 寄存器状态、栈回溯、FreeRTOS任务列表

安全模式:
  - 仅BLE通信 (低功耗)
  - 禁止自主运动
  - 允许远程控制 (限速30%)
  - LED红色闪烁提示
  - 用户可通过APP退出安全模式
```

### 5.2 主脑服务恢复

```
Gateway崩溃:
  - 容器自动重启 (Docker restart policy)
  - 机器人检测到断连 → 自动重连
  - 连接到新Gateway实例 → 重新注册
  - 会话状态丢失 → 机器人发送完整状态

Scheduler崩溃:
  - 热备接管 (Leader election via NATS)
  - 从PostgreSQL恢复任务队列
  - 从Redis恢复机器人状态
  - 未完成任务重新调度

数据库崩溃:
  - PostgreSQL: 主从切换 (自动failover)
  - Redis: Sentinel自动切换
  - NATS: JetStream复制 (R3)
```
