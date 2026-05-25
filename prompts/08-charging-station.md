# 充电站固件实现提示词

## 角色

你是一个嵌入式开发工程师。请根据以下架构文档实现充电站固件。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/hardware-form-factor.md` §4 — 充电站设计

## 硬件

```
MCU: ESP32-C3 (低成本, 足够)
外设:
  - 红外LED × 3 (940nm, 38kHz调制)
  - 充电控制继电器/MOSFET
  - LED状态指示 (红/绿)
  - 电流检测 (ACS712)
```

## 项目结构

```
firmware/charging-station/
├── CMakeLists.txt
├── main/
│   ├── main.c
│   ├── config.h
│   ├── ir_beacon.c      — 红外信标发射
│   ├── charge_ctrl.c    — 充电控制
│   └── include/
```

## 关键实现

### 1. 红外信标

```c
// 38kHz载波调制
#define IR_CARRIER_FREQ    38000
#define IR_LED_GPIO        2
#define BEACON_ID          0x01  // 充电站ID

// RMT外设生成38kHz调制
void ir_beacon_init(void) {
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(IR_LED_GPIO, RMT_CHANNEL_0);
    config.clk_div = 80; // 80MHz/80 = 1MHz
    rmt_config(&config);
    rmt_driver_install(config.channel, 0, 0);
}

// 发射信标帧: [前导码 4ms] [充电站ID 8bit] [校验 4bit]
void ir_beacon_send(void) {
    rmt_item32_t items[32];
    int idx = 0;

    // 前导码: 4ms 38kHz脉冲
    items[idx++] = (rmt_item32_t){{1, 1, 152, 0}}; // 152 ticks = 4ms @ 38kHz

    // 数据: 充电站ID (8bit) + 校验 (4bit)
    uint8_t data = BEACON_ID;
    uint8_t checksum = (data >> 4) ^ (data & 0x0F);
    uint16_t frame = (data << 4) | checksum;

    for (int i = 11; i >= 0; i--) {
        if ((frame >> i) & 1) {
            items[idx++] = (rmt_item32_t){{1, 1, 56, 0}}; // 1 = 1.5ms
        } else {
            items[idx++] = (rmt_item32_t){{1, 1, 28, 0}}; // 0 = 0.75ms
        }
    }

    rmt_write_items(RMT_CHANNEL_0, items, idx, true);
}

// 3个信标相位偏移 (0°/120°/240°)
// 通过定时器控制发射时序
void beacon_phase_control(void *arg) {
    while (1) {
        ir_beacon_send();
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms周期
    }
}
```

### 2. 充电控制

```c
#define CHARGE_MOSFET_GPIO  3
#define CURRENT_SENSE_ADC   ADC1_CHANNEL_0
#define CHARGE_VOLTAGE_MV   5000
#define CHARGE_CURRENT_MA   1000

typedef enum {
    CHARGE_IDLE,
    CHARGE_ACTIVE,
    CHARGE_COMPLETE,
    CHARGE_ERROR,
} charge_state_t;

void charge_control_task(void *arg) {
    charge_state_t state = CHARGE_IDLE;

    while (1) {
        int current_ma = read_current_sense();

        switch (state) {
            case CHARGE_IDLE:
                // 检测到机器人接触 (电流>10mA)
                if (current_ma > 10) {
                    gpio_set_level(CHARGE_MOSFET_GPIO, 1);
                    state = CHARGE_ACTIVE;
                    led_set_color(0, 255, 0); // 绿色
                }
                break;

            case CHARGE_ACTIVE:
                // 充电完成检测 (电流<50mA)
                if (current_ma < 50) {
                    state = CHARGE_COMPLETE;
                    led_set_color(0, 0, 255); // 蓝色
                }
                // 过流保护
                if (current_ma > CHARGE_CURRENT_MA * 1.2) {
                    gpio_set_level(CHARGE_MOSFET_GPIO, 0);
                    state = CHARGE_ERROR;
                    led_set_color(255, 0, 0); // 红色
                }
                break;

            case CHARGE_COMPLETE:
                // 机器人离开 (电流<5mA)
                if (current_ma < 5) {
                    state = CHARGE_IDLE;
                    led_set_color(0, 0, 0);
                }
                break;

            case CHARGE_ERROR:
                // 等待复位
                vTaskDelay(pdMS_TO_TICKS(5000));
                state = CHARGE_IDLE;
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

## 约束

- 红外信标周期: 50ms
- 38kHz载波, PPM调制
- 充电电流: 1A max
- 过流保护: 1.2A
- 充电完成检测: 电流<50mA
- 状态LED: 红(错误)/绿(充电)/蓝(完成)/灭(空闲)
