# 集束指令协议规范 (Bundle Command Protocol — BCP)

## 1. 设计目标

- **极简**: 最小化传输字节数，适配ESP32有限内存
- **快速解析**: 定长字段优先，避免动态解析
- **集束**: 多条指令打包成单帧，减少通信往返
- **可靠**: 支持序列号、确认、重传

## 2. 帧结构

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Magic(1B)  |   Version(1B) |        TotalLen(2B)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     SeqNo(2B) |  CmdCount(1B) |        Reserved(1B)           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Commands...                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         CRC16(2B)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

Magic:    0xCB (Companion Bot)
Version:  0x01
TotalLen: 整帧长度(含头部+CRC)，最大 1024 字节
SeqNo:    帧序列号，用于确认和重传
CmdCount: 帧内指令数量 (1-32)
CRC16:    CRC-16/CCITT 校验
```

## 3. 指令结构 (Command)

每条指令紧跟在帧头之后，连续排列：

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  CmdID(2B)    |  PayloadLen(1B)|  Payload...  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

CmdID:      指令类型枚举
PayloadLen: 本条指令的payload长度 (0-255 字节)
Payload:    指令参数
```

## 4. 指令集枚举

### 4.1 系统指令 (0x00xx)

| CmdID | 名称 | 方向 | Payload | 说明 |
|-------|------|------|---------|------|
| 0x0001 | HEARTBEAT | R→M | [状态(1B), 电量(1B), RSSI(1B), 任务ID(2B)] | 心跳 |
| 0x0002 | REGISTER | R→M | [能力集(NB), 固件版本(4B)] | 注册 |
| 0x0003 | REG_ACK | M→R | [短ID(2B), 心跳间隔(2B)] | 注册确认 |
| 0x0004 | PING | 双向 | [时间戳(4B)] | 延迟测量 |
| 0x0005 | PONG | 双向 | [时间戳(4B)] | 延迟响应 |
| 0x0006 | RESET | M→R | [原因码(1B)] | 远程重启 |
| 0x0007 | OTA_START | M→R | [大小(4B), MD5(16B)] | OTA开始 |
| 0x0008 | OTA_CHUNK | M→R | [偏移(4B), 数据(NB)] | OTA分块 |
| 0x0009 | OTA_DONE | M→R | [] | OTA完成，校验重启 |

### 4.2 运动控制指令 (0x01xx)

| CmdID | 名称 | Payload | 说明 |
|-------|------|---------|------|
| 0x0101 | MOVE | [方向(1B), 速度(1B)] | 方向: 0=前,1=后,2=左,3=右,4=旋转左,5=旋转右; 速度: 0-255 |
| 0x0102 | MOVE_TO | [X(2B), Y(2B), 速度(1B)] | 坐标导航(相对位置, cm) |
| 0x0103 | STOP | [急停标志(1B)] | 0=缓停, 1=急停 |
| 0x0104 | SERVO_SET | [舵机ID(1B), 角度(2B)] | 舵机控制 0-180° |
| 0x0105 | SERVO_BATCH | [count(1B), {id,angle}×N] | 批量舵机 |
| 0x0106 | HEAD_PAN_TILT | [pan(1B), tilt(1B)] | 头部云台 0-180° |

### 4.3 表达指令 (0x02xx)

| CmdID | 名称 | Payload | 说明 |
|-------|------|---------|------|
| 0x0201 | LED_SET | [LED_ID(2B), R(1B), G(1B), B(1B)] | 单LED颜色 |
| 0x0202 | LED_PATTERN | [模式(1B), 速度(1B), R,G,B] | 灯效: 0=呼吸,1=流水,2=闪烁,3=彩虹 |
| 0x0203 | LED_OFF | [] | 全灭 |
| 0x0204 | FACE_EXPR | [表情ID(1B)] | 0=中性,1=开心,2=悲伤,3=惊讶,4=生气,5=困惑 |
| 0x0205 | FACE_CUSTOM | [帧数据(NB)] | 自定义表情帧 |
| 0x0206 | SPEAK | [音量(1B), 音频格式(1B), 数据(NB)] | 播报音频 |
| 0x0207 | TTS_TEXT | [编码(1B), 文本(NB)] | TTS文本(主脑合成后下发音频) |

### 4.4 感知数据上报 (0x03xx)

| CmdID | 名称 | Payload | 说明 |
|-------|------|---------|------|
| 0x0301 | ENV_DATA | [temp(2B), humi(2B), press(4B), light(4B), air(2B)] | 环境数据 |
| 0x0302 | MOTION_EVENT | [检测类型(1B), 置信度(1B)] | 运动检测 |
| 0x0303 | AUDIO_EVENT | [事件类型(1B), 能量(2B)] | 0=VAD开始,1=VAD结束,2=唤醒词 |
| 0x0304 | AUDIO_STREAM | [编码(1B), 数据(NB)] | 音频流(Opus/PCM) |
| 0x0305 | IMAGE_SNAPSHOT | [格式(1B), 宽(2B), 高(2B), 数据(NB)] | JPEG快照 |
| 0x0306 | DEPTH_DATA | [宽(2B), 高(2B), 数据(NB)] | 深度图 |
| 0x0307 | TOUCH_EVENT | [区域ID(1B), 压力(1B), 状态(1B)] | 触觉事件 |
| 0x0308 | IMU_DATA | [ax,ay,az,gx,gy,gz(各2B)] | IMU原始数据 |
| 0x0309 | OBSTACLE | [方向(1B), 距离(2B)] | 避障报告 |

### 4.5 集群指令 (0x04xx)

| CmdID | 名称 | Payload | 说明 |
|-------|------|---------|------|
| 0x0401 | TASK_ASSIGN | [任务类型(1B), 优先级(1B), 参数(NB)] | 任务分配 |
| 0x0402 | TASK_STATUS | [任务ID(2B), 状态(1B), 进度(1B)] | 任务进度 |
| 0x0403 | TASK_CANCEL | [任务ID(2B)] | 取消任务 |
| 0x0404 | SWARM_FORM | [队形ID(1B), 坐标列表(NB)] | 编队指令 |
| 0x0405 | PEER_MSG | [目标短ID(2B), 数据(NB)] | 机器人间消息 |

## 5. 编解码规则

### 5.1 字节序
- 所有多字节字段: **小端序 (Little-Endian)**

### 5.2 示例: 编码一帧

组装一条运动指令 + LED指令:

```
帧头:
  Magic     = 0xCB
  Version   = 0x01
  TotalLen  = 16  (8帧头 + 4指令1 + 2指令2 + 2CRC)
  SeqNo     = 0x0042
  CmdCount  = 2
  Reserved  = 0x00

指令1 - MOVE前进半速:
  CmdID     = 0x0101
  PayloadLen= 2
  Payload   = [0x00, 0x80]  // 方向前, 速度128

指令2 - LED流水灯:
  CmdID     = 0x0202
  PayloadLen= 5
  Payload   = [0x01, 0x20, 0xFF, 0x00, 0x00]  // 流水, 中速, 红色

CRC16 = (计算以上所有字节)
```

十六进制:
```
CB 01 10 00 42 00 02 00
01 01 02 00 80
02 02 05 01 20 FF 00 00
[CRC_L] [CRC_H]
```

### 5.3 解析流程 (ESP32端)

```c
// 伪代码
int bcp_parse(const uint8_t *buf, size_t len) {
    if (len < 8) return -1;                // 最小帧长度
    if (buf[0] != 0xCB) return -2;         // Magic校验
    if (buf[1] != 0x01) return -3;         // 版本校验

    uint16_t total = buf[2] | (buf[3]<<8);
    if (len < total) return -4;            // 数据不完整
    if (crc16(buf, total-2) != *(uint16_t*)(buf+total-2))
        return -5;                         // CRC校验失败

    uint8_t cmd_count = buf[6];
    const uint8_t *p = buf + 8;            // 跳过帧头

    for (int i = 0; i < cmd_count; i++) {
        uint16_t cmd_id = p[0] | (p[1]<<8);
        uint8_t  plen   = p[2];
        const uint8_t *payload = p + 3;
        dispatch(cmd_id, payload, plen);   // 分发到对应handler
        p += 3 + plen;
    }
    return 0;
}
```

## 6. 流控与重传

### 6.1 确认机制

- 每帧的 SeqNo 接收方需要确认
- 确认帧: 复用帧头结构，CmdCount=0，SeqNo=被确认的帧的SeqNo
- 发送方维护滑动窗口(窗口大小=8)

```
发送方                          接收方
  │── Frame(Seq=1) ──────────►│
  │── Frame(Seq=2) ──────────►│
  │◄── ACK(Seq=1) ────────────│
  │── Frame(Seq=3) ──────────►│
  │◄── ACK(Seq=2) ────────────│
```

### 6.2 超时重传

```
重传超时 (RTO): 初始 200ms, 基于RTT自适应
最大重传次数: 3
重传策略: 指数退避 200ms → 400ms → 800ms

紧急指令 (如STOP): 不等待确认，连续发送3次
```

### 6.3 流控

```
接收方维护缓冲区水位:
  - 正常: 缓冲区 < 50%  → 发送 WINDOW_UPDATE
  - 警告: 缓冲区 > 75%  → 暂停 WINDOW_UPDATE
  - 危险: 缓冲区 > 90%  → 发送 PAUSE 指令

发送方:
  - 收到 PAUSE → 停止发送非紧急指令
  - 收到 WINDOW_UPDATE → 恢复发送
```

## 7. 优先级队列

```
优先级 0 (最高): STOP、紧急安全指令 → 立即发送，不排队
优先级 1 (高):   运动控制、舵机 → 10ms内发送
优先级 2 (中):   LED、表情、音频 → 50ms内发送
优先级 3 (低):   环境数据上报、OTA → 尽力发送

每帧内指令按优先级排序，高优先级指令优先打包
```

## 8. 优化策略

### 8.1 差量心跳

```
首次心跳: 完整状态 [短ID(2B) + 状态(1B) + 电量(1B) + RSSI(1B) + 任务ID(2B)] = 7B
后续心跳: 只发变化量
  - 无变化: [短ID(2B) + 0xFF] = 3B
  - 仅电量变化: [短ID(2B) + 电量(1B)] = 3B
  - 全变: 完整包 7B

典型场景节省 60% 心跳带宽
```

### 8.2 连续指令免确认

```
运动控制指令设置 NO_ACK 标志(帧头Reserved bit0):
  - 主脑连续下发 MOVE 指令 (10Hz)
  - 机器人收到后立即执行, 不发ACK
  - 主脑通过心跳确认机器人还在

必须ACK的指令:
  - REGISTER / REG_ACK
  - OTA_START / OTA_DONE
  - TASK_ASSIGN / TASK_CANCEL
  - STOP (紧急停止)
  - CONFIG变更
```

### 8.3 压缩策略

```
场景优化:
  - 环境数据: delta编码(只发变化量)
  - 音频流: Opus编码(64kbps, 延迟20ms)
  - 图像: JPEG压缩, 质量可调(30-80)
  - 深度图: 高度压缩或只发关键点

集束优化:
  - 同周期内的多个传感器数据打包成单帧
  - 批量舵机指令合并
  - LED批量更新
```
