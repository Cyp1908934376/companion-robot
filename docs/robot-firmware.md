# 机器人固件架构 — ESP32-S3

## 1. 硬件平台

```
ESP32-S3-WROOM-1 (N16R8)
├── 双核 Xtensa LX7 @ 240MHz
├── 512KB SRAM + 8MB PSRAM
├── 16MB Flash
├── WiFi 802.11 b/g/n
├── BLE 5.0
└── NPU (向量指令加速)

外设连接 (Phase 1 初代 30-40cm MiniBot):
├── I2S0 → 麦克风阵列 (INMP441 × 4)
├── I2S1 → 扬声器 (MAX98357A DAC)
├── SPI0 → 摄像头 (OV2640)
├── I2C0 → IMU (BMI270) + 环境 (BME280+SGP30+BH1750)
├── Touch → 电容触摸 (ESP32-S3内置触摸引脚, 躯干2-4个触摸区)
├── UART0 → 调试串口
├── RMT → WS2812B LED环 × 8
├── MCPWM → 电机驱动 (DRV8833) × 2 → N20电机 × 4 (麦克纳姆轮)
├── LEDC → 舵机 × 2 (头部 pan + tilt)
└── GPIO → ToF避障 (VL53L0X × 3, I2C地址切换)

演进外设 (Phase 2+):
├── SPI1 → 深度传感器 (VL53L5CX, Phase 2+)
├── I2C1 → 触觉传感器阵列 (MPR121, Phase 2+)
├── MCPWM → 机械臂舵机 × 4 (Phase 2+)
└── GPIO → 超声波 HC-SR04 × 3 (Phase 3+)
```

## 2. 软件分层

```
┌─────────────────────────────────────────────────────┐
│                    应用层 (App)                       │
│  行为引擎 / 对话客户端 / 任务执行器                    │
├─────────────────────────────────────────────────────┤
│                    服务层 (Service)                    │
│  感知融合 / 运动规划 / 表达管理 / 音频管理              │
├─────────────────────────────────────────────────────┤
│                    通信层 (Comm)                       │
│  BCP协议栈 / 连接管理器 / 模式切换 / 加密              │
├─────────────────────────────────────────────────────┤
│                    驱动层 (Driver)                     │
│  摄像头 / 麦克风 / IMU / 电机 / LED / I2C / SPI      │
├─────────────────────────────────────────────────────┤
│                    系统层 (ESP-IDF / FreeRTOS)         │
│  任务调度 / 内存管理 / WiFi/BLE栈 / OTA / NVS         │
└─────────────────────────────────────────────────────┘
```

## 3. FreeRTOS 任务分配

### 3.1 核心绑定

```
Core 0 (Protocol Core):
├── task_comm_rx       (优先级 5)  — 通信接收 + BCP解码
├── task_comm_tx       (优先级 4)  — 通信发送 + BCP编码
├── task_ble_handler   (优先级 3)  — BLE事件处理
└── task_wifi_handler  (优先级 3)  — WiFi事件处理

Core 1 (Sensor/Actuator Core):
├── task_vision        (优先级 4)  — 摄像头采集 + 本地推理
├── task_audio_in      (优先级 5)  — 麦克风采集 + VAD
├── task_audio_out     (优先级 3)  — 扬声器播放
├── task_imu           (优先级 3)  — IMU采样 + 姿态融合
├── task_motor_ctrl    (优先级 5)  — 电机PID控制 (100Hz)
├── task_servo_ctrl    (优先级 3)  — 舵机平滑控制
├── task_env_sensor    (优先级 1)  — 环境传感器周期采样
├── task_touch         (优先级 4)  — 触觉中断处理
├── task_led           (优先级 2)  — LED动画更新
└── task_behavior      (优先级 2)  — 行为状态机
```

### 3.2 任务间通信

```
队列:
  q_cmd_incoming    (深度 16)  — BCP解码后的指令队列
  q_cmd_outgoing    (深度 32)  — 待发送的BCP帧队列
  q_sensor_event    (深度 8)   — 传感器事件队列
  q_audio_frame     (深度 4)   — 音频帧队列
  q_vision_frame    (深度 2)   — 视觉帧队列

信号量:
  sem_motor_lock    — 电机控制互斥
  sem_i2c_bus       — I2C总线互斥
  sem_spi_bus       — SPI总线互斥

事件组:
  evt_conn_state    — 连接状态事件
  evt_sensor_ready  — 传感器就绪事件
  evt_emergency     — 紧急事件
```

### 3.3 内存预算

```
总计 512KB SRAM + 8MB PSRAM:

SRAM 分配:
  FreeRTOS内核      ~32KB
  WiFi/BLE栈        ~80KB
  通信层缓冲区       ~32KB
  BCP编解码缓冲      ~4KB
  传感器驱动         ~16KB
  任务栈 (12个任务)   ~48KB
  系统保留           ~100KB
  剩余可用           ~200KB

PSRAM 分配:
  视觉帧缓冲        ~1MB (VGA JPEG)
  音频缓冲           ~128KB
  OTA缓冲           ~512KB
  动态分配池         ~6MB
```

## 4. 感知子系统

### 4.1 视觉系统

```
摄像头采集 (Core 1, task_vision):
├── 模式1: 连续流 (30fps QVGA) — 用于远程监控
├── 模式2: 快照 (VGA JPEG on demand) — 主脑请求时
└── 模式3: 本地检测 (低分辨率 + 运动检测)

本地视觉处理:
├── 运动检测: 帧差法, 阈值触发
│   └── 检测到运动 → 通知主脑 → 主脑决定是否抓拍
├── 人脸检测: Haar级联 (ESP32-S3 NPU加速)
│   └── 检测到人脸 → 上报坐标 + 抓拍
└── 深度处理: VL53L5CX 8×8区域深度
    └── 障碍物检测 → 本地避障决策

性能指标:
  运动检测: <20ms @ QVGA
  人脸检测: ~100ms @ QVGA (NPU加速)
  JPEG编码: ~50ms @ VGA
```

### 4.2 音频系统

```
麦克风采集 (Core 1, task_audio_in):
├── 4通道 I2S 采样 (16kHz, 16bit)
├── 波束成形: 简单延迟求和 (声源方向估计)
├── AEC: 基于参考信号的回声消除
├── VAD: 能量+过零率双阈值
│   └── VAD激活 → 开始录音 → VAD结束 → 编码发送
└── 唤醒词: 本地小模型 (ESP-SR)
    └── 唤醒词触发 → 上报事件 → 进入对话模式

音频编码:
  默认: Opus @ 16kHz, 64kbps, 20ms帧
  备选: PCM 16bit (低延迟场景)

输出 (Core 1, task_audio_out):
├── I2S DAC 输出
├── 音量控制 (软件混音)
└── 支持格式: PCM, Opus解码
```

### 4.3 环境系统

```
task_env_sensor (1Hz周期):
├── BME280: 温度(±0.5°C), 湿度(±3%), 气压(±1hPa)
├── BH1750: 光照(1-65535 lux)
├── SGP30: TVOC(ppb), eCO2(ppm)
└── 打包为 ENV_DATA 指令, 附带在心跳中上报

task_touch (Phase 1 简化版):
├── ESP32-S3 内置触摸引脚 × 4 (躯干外壳触摸区)
├── PCB铜箔走线 + 导电布覆盖
├── 中断驱动, 去抖 50ms
├── 检测: 轻触/长触(>500ms)/滑动(相邻引脚时序)
└── 上报 TOUCH_EVENT 指令

task_touch (Phase 2+ 完整版):
├── MPR121 × 2 = 24个触觉通道
├── FSR压力传感器阵列 (拥抱检测)
├── 压力分级: 轻触/正常/用力 (基于电容变化量)
└── 上报 TOUCH_EVENT 指令

task_imu:
├── BMI270: 6轴 (加速度±16g, 陀螺仪±2000°/s)
├── 采样率: 200Hz
├── 姿态融合: 互补滤波 (低CPU开销)
│   └── 或 Madgwick滤波 (精度更高, CPU占用~5%)
└── 上报: 事件驱动(跌倒检测/碰撞) + 周期上报(IMU_DATA)
```

### 4.4 避障系统

```
task_motor_ctrl 中集成:
├── VL53L0X × 3: 前方/左方/右方 (量程2m, 精度±3%)
│   └── 采样率: 50Hz (3路I2C地址切换轮询)
├── 超声波 × 3: (Phase 3+ 增加, 量程4m确认)
│   └── 触发式, ToF检测到近距障碍时启用确认
└── 融合决策:
    ├── 距离 > 50cm: 正常通行
    ├── 30-50cm: 减速 + 上报
    ├── 10-30cm: 停止 + 上报
    └── < 10cm: 急停 + 后退 + 紧急上报
    └── Phase 1: 仅3×ToF, 阈值适当保守(50cm开始减速)
```

## 5. 电源管理

### 5.1 功耗预算

```
模块              典型功耗    峰值功耗
──────────────────────────────────────
ESP32-S3 (WiFi)   240mA      350mA
ESP32-S3 (BLE)    80mA       120mA
ESP32-S3 (Light Sleep)  0.8mA      -
摄像头 OV2640     40mA       60mA
麦克风 × 4        10mA       15mA
IMU BMI270        1mA        3mA
环境传感器         2mA        5mA
ToF × 3           15mA       30mA
OLED屏            20mA       40mA
LED环 × 8         10mA       480mA (全白)
N20电机 × 4       200mA      800mA (堵转)
舵机 × 2          200mA      400mA
扬声器            50mA       300mA
──────────────────────────────────────
活跃总计:         ~800mA     ~2.5A
待机(仅WiFi):     ~300mA
深度睡眠:         ~10μA

2000mAh电池:
  活跃续航: ~2.5h
  空闲续航: ~6.5h
  深度睡眠: ~8天
```

### 5.2 电源状态机

```c
typedef enum {
    POWER_ACTIVE,       // 全功能
    POWER_IDLE,         // 关闭摄像头, CPU降频80MHz, 保持WiFi
    POWER_LIGHT_SLEEP,  // WiFi省电模式, RTC定时唤醒心跳
    POWER_DEEP_SLEEP,   // 仅BLE, RTC 60s唤醒, ULP采样
    POWER_SHUTDOWN,     // 断电, 仅RTC, 按键唤醒
    POWER_LOW_BATTERY,  // 低电量保护
} power_state_t;

// 状态转换
// ACTIVE --(5min无任务)--> IDLE
// IDLE --(30min无事件)--> LIGHT_SLEEP
// LIGHT_SLEEP --(30min无事件)--> DEEP_SLEEP
// 任意状态 --(收到指令)--> ACTIVE
// 任意状态 --(电量<10%)--> LOW_BATTERY → DEEP_SLEEP
// 任意状态 --(电量<3%)--> SHUTDOWN

// 唤醒源
// ACTIVE: 主脑指令, 唤醒词, 触摸, 运动, 定时器
// LIGHT_SLEEP: WiFi DTIM, RTC定时, GPIO中断
// DEEP_SLEEP: RTC(60s), GPIO(按键), ULP(传感器阈值)
```

### 5.3 充电管理

```
硬件:
  - USB-C充电 (TP4056, 500mA)
  - 桌面充电站 (弹簧针触点, 1A) — 详见 hardware-form-factor.md §4
  - 电量计 MAX17048 (I2C, ±5%精度)
  - 红外接收头 × 3 (朝前扇形, 用于信标定位)

充电触发:
  - 电量 < 20%: 警告, 主脑安排充电任务
  - 电量 < 10%: 紧急, 中断当前任务, 自主寻路
  - 空闲 > 30min + 电量 < 50%: 主动充电
  - 主脑下发充电指令

寻路充电流程:
  1. 检测低电量 → 上报主脑
  2. 主脑确认 → 下发 TASK_ASSIGN(回充电站)
  3. 头部云台pan扫描 (0-180°)
  4. 3个红外接收头检测信标RSSI → 计算方位角
  5. 转向朝向充电站 → 前进
  6. ToF保持前方安全距离
  7. 检测到导向槽 (ToF距离突变) → 减速滑入
  8. 底部触点接触 → 充电电路导通 → 上报充电中
  9. 电量 > 80% → 上报完成 → 退出充电站

充电状态:
  CHARGING_IDLE     — 未充电
  CHARGING_SEEKING  — 寻路中
  CHARGING_DOCKING  — 对接中
  CHARGING_ACTIVE   — 充电中
  CHARGING_COMPLETE — 充满
  CHARGING_FAILED   — 对接失败 (重试3次后上报)
```

## 6. 通信管理层

### 5.1 连接管理器

```c
// 连接状态机
typedef enum {
    CONN_STATE_INIT,
    CONN_STATE_WIFI_CONNECTING,
    CONN_STATE_WIFI_CONNECTED,
    CONN_STATE_WS_CONNECTING,
    CONN_STATE_WS_CONNECTED,    // 直连模式
    CONN_STATE_BLE_ADVERTISING,
    CONN_STATE_BLE_CONNECTED,   // 桥接模式
    CONN_STATE_RELAY_MODE,      // 转发模式
    CONN_STATE_DISCONNECTED,
    CONN_STATE_ERROR,
} conn_state_t;

// 模式切换逻辑
void conn_manager_tick() {
    switch (state) {
        case CONN_STATE_INIT:
            // 1. 尝试WiFi连接 (从NVS读取配置)
            // 2. WiFi成功 → 尝试WebSocket
            // 3. WiFi失败 → 开BLE广播
            break;

        case CONN_STATE_WS_CONNECTED:
            // 直连模式正常运行
            // 检测WebSocket断连 → 切到WIFI_CONNECTED重试
            break;

        case CONN_STATE_BLE_CONNECTED:
            // 桥接模式, 等待移动设备转发
            // 同时周期性尝试WiFi (5分钟间隔)
            break;

        case CONN_STATE_DISCONNECTED:
            // 指数退避重连: 1s → 2s → 4s → 8s → 16s → 30s(max)
            break;
    }
}
```

### 5.2 BCP协议栈

```
发送路径:
  应用层 → cmd_enqueue() → 优先级排序 → BCP编码 → 帧队列 → WebSocket/BLE发送

接收路径:
  WebSocket/BLE接收 → 帧校验 → BCP解码 → 指令分发 → 任务队列

关键函数:
  bcp_encode(cmd_list, buf, max_len)  → 编码集束帧
  bcp_parse(buf, len, cmd_list)       → 解析集束帧
  bcp_dispatch(cmd_id, payload, len)  → 指令分发
  bcp_ack(seq_no)                     → 发送确认
  bcp_retransmit_check()              → 重传检查
```

### 5.3 BLE GATT 服务定义

```
Service UUID: 0xCB00 (Companion Bot)

Characteristic          UUID      属性        说明
─────────────────────────────────────────────────────
BCP_TX                  0xCB01    Write       主脑→机器人 (Write/WriteNoRsp)
BCP_RX                  0xCB02    Notify      机器人→主脑 (Notify)
BCP_CONTROL             0xCB03    Read/Write  连接控制 (MTU协商, 模式切换)
DEVICE_INFO             0xCB04    Read        设备信息 (机器码, 能力集, 固件版本)
OTA_DATA                0xCB05    Write       OTA数据传输

MTU协商:
  默认: 23字节 (BLE最小)
  目标: 247字节 (ESP32 BLE最大推荐)
  集束帧超过MTU时自动分片, 用帧头Reserved字段标记分片序号
```

## 6. 启动流程

```
boot
 │
 ├─ 1. 硬件初始化 (时钟, GPIO, 外设)
 ├─ 2. NVS读取配置 (WiFi SSID/密码, 主脑地址, 机器人名称)
 ├─ 3. 驱动初始化 (I2C/SPI总线, 传感器探测)
 ├─ 4. WiFi连接 (超时10s)
 │    ├─ 成功 → mDNS发现主脑 / 直连配置地址
 │    └─ 失败 → 启动BLE广播
 ├─ 5. BCP握手 (REGISTER → REG_ACK)
 ├─ 6. 启动传感器任务
 ├─ 7. 启动行为引擎 (idle状态)
 └─ 8. 进入主循环
```

## 7. 离线行为

### 7.1 行为包结构

```
主脑预下发, 机器人本地缓存, 网络断开时自动启用:

存储:
  NVS分区:   行为配置 JSON (~10KB)
  Flash分区:  预录制音频 (~1MB)
  PSRAM:      运行时行为状态

行为包内容:
  - 基础巡逻: 预设路线点序列
  - 充电回家: 记录充电座位置, 低电量自动导航
  - 避障反应: 已有, 无需额外
  - 简单对话: 预录制问答对 (100-200条)
  - 情绪状态机: 开心/无聊/困倦/好奇 (基于时间+事件)
  - 时间触发: 早上问好, 晚上提醒休息

离线对话:
  1. 唤醒词触发 (ESP-SR, 本地)
  2. 本地小词汇识别 (~200词)
  3. 关键词匹配 → 执行对应行为
  4. 无匹配 → 固定回复 + 记录, 上线后同步主脑
```

### 7.2 行为状态机

```
┌────────┐  时间/事件  ┌────────┐  时间/事件  ┌────────┐
│ HAPPY  │ ──────────► │ BORED  │ ──────────► │ SLEEPY │
│ 开心   │ ◄────────── │ 无聊   │             │ 困倦   │
└────────┘  互动/任务   └────────┘             └────────┘
    │                      │                       │
    │ 发现新事物            │ 探索                  │ 低电量
    ▼                      ▼                       │
┌────────┐            ┌────────┐                   │
│CURIOUS │            │EXPLORE │                   │
│ 好奇   │            │ 探索   │ ◄─────────────────┘
└────────┘            └────────┘

每个状态对应不同的LED灯效、表情、主动行为
```

## 8. OTA更新

```
流程:
  1. 主脑下发 OTA_START (固件大小, MD5)
  2. 机器人确认, 预留PSRAM缓冲
  3. 主脑下发 OTA_CHUNK × N (每块512B)
  4. 机器人写入OTA分区, 进度上报 TASK_STATUS
  5. 主脑下发 OTA_DONE
  6. 机器人校验MD5 → 成功则重启到新固件
  7. 新固件启动后发送心跳, 主脑确认版本

安全:
  - 固件签名验证 (Ed25519)
  - 回滚机制: 新固件启动失败 → 自动回退
  - 双分区方案: factory分区不可覆盖
```
