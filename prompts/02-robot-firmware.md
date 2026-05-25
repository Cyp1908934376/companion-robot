# ESP32-S3 机器人固件实现提示词

## 角色

你是一个ESP32嵌入式开发工程师。请根据以下架构文档实现完整的机器人固件。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/robot-firmware.md` — 固件架构（软件分层、FreeRTOS任务、感知子系统）
- `docs/protocol.md` — BCP协议规范
- `docs/hardware-form-factor.md` — 硬件选型与引脚分配
- `docs/error-handling.md` — 错误处理与降级策略

## 项目结构

```
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv          — 分区表 (factory + app + ota + nvs + coredump)
├── main/
│   ├── CMakeLists.txt
│   ├── main.c              — 入口, 硬件初始化, 任务启动
│   ├── config.h            — 引脚定义, 参数配置
│   └── Kconfig.projbuild   — menuconfig选项
├── components/
│   ├── bcp/                — BCP协议栈 (引用crates/bcp-core的C绑定)
│   │   ├── CMakeLists.txt
│   │   ├── bcp_codec.c
│   │   ├── bcp_codec.h
│   │   └── include/
│   ├── comm/               — 通信管理层
│   │   ├── conn_manager.c  — 连接状态机
│   │   ├── ws_client.c     — WebSocket客户端
│   │   ├── ble_gatt.c      — BLE GATT服务
│   │   └── include/
│   ├── perception/         — 感知子系统
│   │   ├── camera.c        — 摄像头驱动
│   │   ├── microphone.c    — I2S麦克风
│   │   ├── imu.c           — BMI270驱动
│   │   ├── env_sensor.c    — BME280+SGP30+BH1750
│   │   ├── tof.c           — VL53L0X驱动
│   │   ├── touch.c         — 电容触摸
│   │   └── include/
│   ├── motion/             — 运动控制
│   │   ├── motor.c         — 电机PID控制
│   │   ├── servo.c         — 舵机控制
│   │   ├── obstacle.c      — 避障决策
│   │   └── include/
│   ├── expression/         — 表达系统
│   │   ├── led.c           — WS2812B驱动
│   │   ├── face.c          — OLED表情
│   │   ├── speaker.c       — I2S扬声器
│   │   └── include/
│   ├── power/              — 电源管理
│   │   ├── power_mgr.c     — 电源状态机
│   │   ├── battery.c       — 电量计MAX17048
│   │   ├── charging.c      — 充电控制
│   │   └── include/
│   ├── behavior/           — 行为引擎
│   │   ├── state_machine.c — 行为状态机
│   │   ├── offline.c       — 离线行为包
│   │   └── include/
│   └── storage/            — 存储
│       ├── nvs_config.c    — NVS配置读写
│       ├── ota.c           — OTA更新
│       └── include/
└── test/
    ├── test_bcp.c          — BCP单元测试
    ├── test_motor.c        — PID控制测试
    └── test_power.c        — 电源状态机测试
```

## 关键实现

### 1. 引脚定义 (config.h)

```c
// I2C
#define I2C_MASTER_SCL_IO    22
#define I2C_MASTER_SDA_IO    21
#define I2C_MASTER_FREQ_HZ   400000

// I2S 麦克风
#define I2S_MIC_BCK_IO       26
#define I2S_MIC_WS_IO        25
#define I2S_MIC_DATA_IO      33

// I2S 扬声器
#define I2S_SPK_BCK_IO       14
#define I2S_SPK_WS_IO        27
#define I2S_SPK_DATA_IO      13

// 电机 (DRV8833)
#define MOTOR_L1_A_IO        32
#define MOTOR_L1_B_IO        33
#define MOTOR_L2_A_IO        25
#define MOTOR_L2_B_IO        26
#define MOTOR_R1_A_IO        27
#define MOTOR_R1_B_IO        14
#define MOTOR_R2_A_IO        12
#define MOTOR_R2_B_IO        13

// 舵机
#define SERVO_PAN_IO         18
#define SERVO_TILT_IO        19

// LED
#define LED_DATA_IO          5
#define LED_COUNT            8

// ToF (I2C地址切换)
#define TOF_XSHUT_1_IO       34
#define TOF_XSHUT_2_IO       35
#define TOF_XSHUT_3_IO       36

// 摄像头 (SPI)
#define CAM_SCK_IO           40
#define CAM_MISO_IO          41
#define CAM_MOSI_IO          42
#define CAM_CS_IO            44

// 触摸
#define TOUCH_ZONE_1         T9   // GPIO32
#define TOUCH_ZONE_2         T8   // GPIO33
#define TOUCH_ZONE_3         T7   // GPIO27
#define TOUCH_ZONE_4         T6   // GPIO14
```

### 2. FreeRTOS任务创建 (main.c)

```c
void app_main(void) {
    // 硬件初始化
    nvs_flash_init();
    i2c_master_init();
    i2s_driver_install();
    spi_bus_init();

    // 创建队列
    q_cmd_incoming = xQueueCreate(16, sizeof(bcp_cmd_t));
    q_cmd_outgoing = xQueueCreate(32, sizeof(bcp_frame_t));
    q_sensor_event = xQueueCreate(8, sizeof(sensor_event_t));

    // Core 0: 通信
    xTaskCreatePinnedToCore(task_comm_rx, "comm_rx", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(task_comm_tx, "comm_tx", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(task_conn_manager, "conn_mgr", 4096, NULL, 3, NULL, 0);

    // Core 1: 感知+执行
    xTaskCreatePinnedToCore(task_vision, "vision", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_audio_in, "audio_in", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_imu, "imu", 2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(task_motor_ctrl, "motor", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_env_sensor, "env", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(task_touch, "touch", 2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_led, "led", 2048, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(task_behavior, "behavior", 4096, NULL, 2, NULL, 1);
}
```

### 3. 连接状态机 (conn_manager.c)

```c
// 实现 docs/robot-firmware.md §6.1 的连接管理器
// 优先级: WiFi直连 > BLE桥接 > 降级模式
// WiFi断开5s → 尝试BLE
// BLE断开10s → 降级模式
// WiFi恢复 → 立即切回
```

### 4. 电机PID控制 (motor.c)

```c
typedef struct {
    float kp, ki, kd;
    float setpoint, integral, prev_error;
    int output;
} pid_ctrl_t;

// 100Hz控制循环
void task_motor_ctrl(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        // 读取编码器
        // 计算PID
        // 更新PWM
        // 检查堵转
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}
```

### 5. 电源状态机 (power_mgr.c)

```c
// 实现 docs/robot-firmware.md §5.2 的电源状态机
// ACTIVE → IDLE (5min无任务)
// IDLE → LIGHT_SLEEP (30min无事件)
// LIGHT_SLEEP → DEEP_SLEEP (30min无事件)
// 任意 → LOW_BATTERY (电量<10%)
```

## 约束

- ESP-IDF v5.x + FreeRTOS
- Core 0: 通信任务, Core 1: 感知/执行任务
- 看门狗: 每个关键任务2s内喂狗
- 内存: SRAM 512KB, PSRAM 8MB
- BCP帧最大1024字节
- 电机PID控制频率: 100Hz
- 传感器采样: IMU 200Hz, 环境 1Hz, ToF 50Hz
