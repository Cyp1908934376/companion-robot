# 测试策略

## 1. 测试金字塔

```
                    ┌───────────┐
                    │  E2E测试   │  少量, 贵, 慢
                    │ (5-10个)   │
                    ├───────────┤
                  │   集成测试    │  中量, 中速
                  │  (30-50个)   │
                  ├─────────────┤
                │    单元测试      │  大量, 便宜, 快
                │  (200-500个)    │
                └─────────────────┘
```

## 2. 固件测试 (ESP32-S3)

### 2.1 单元测试

```
框架: ESP-IDF Unity + CMock (内置)

测试目标:
  bcp_parse()        — BCP帧解析 (各种正常/异常输入)
  bcp_encode()       — BCP帧编码 (校验字节对齐)
  bcp_dispatch()     — 指令分发 (各指令ID路由正确)
  pid_control()      — PID控制器 (阶跃响应、饱和)
  power_state_machine() — 电源状态转换 (各条件触发)
  conn_manager()     — 连接状态机 (模式切换)
  behavior_engine()  — 行为状态机 (事件触发)

示例测试用例 (BCP解析):
  TEST_CASE("bcp_parse valid heartbeat", "[bcp]") {
      uint8_t frame[] = {0xCB, 0x01, 0x0F, 0x00, 0x01, 0x00, 0x01, 0x00,
                          0x01, 0x01, 0x05, 0x64, 0xC8, 0x0A, 0x00,
                          0xXX, 0xXX}; // CRC
      bcp_cmd_t cmds[8];
      int count = 0;
      int ret = bcp_parse(frame, sizeof(frame), cmds, &count);
      TEST_ASSERT_EQUAL(0, ret);
      TEST_ASSERT_EQUAL(1, count);
      TEST_ASSERT_EQUAL(0x0001, cmds[0].cmd_id);
      TEST_ASSERT_EQUAL(5, cmds[0].payload_len);
  }

  TEST_CASE("bcp_parse bad magic", "[bcp]") {
      uint8_t frame[] = {0xCC, 0x01, ...};
      int ret = bcp_parse(frame, sizeof(frame), cmds, &count);
      TEST_ASSERT_EQUAL(-2, ret);
  }

  TEST_CASE("bcp_parse bad crc", "[bcp]") {
      uint8_t frame[] = {0xCB, 0x01, ...};
      frame[sizeof(frame)-1] ^= 0xFF; // 破坏CRC
      int ret = bcp_parse(frame, sizeof(frame), cmds, &count);
      TEST_ASSERT_EQUAL(-5, ret);
  }

运行方式:
  idf.py -T components/bcp/test build
  idf.py -T components/bcp/test flash monitor
```

### 2.2 硬件在环测试 (HIL)

```
需要: ESP32-S3开发板 + 传感器测试夹具

测试项目:
  ┌────────────────┬──────────────────────────────────────┐
  │ 测试项         │ 方法                                 │
  ├────────────────┼──────────────────────────────────────┤
  │ I2C总线        │ 挂载所有传感器, 逐一读取WHO_AM_I寄存器│
  │ SPI摄像头      │ 采集一帧, 校验JPEG头+尺寸            │
  │ I2S麦克风      │ 录音1s, 检查采样率+幅度              │
  │ I2S扬声器      │ 播放测试音, 人工确认                 │
  │ 电机           │ 正转/反转/停止, 编码器反馈           │
  │ 舵机           │ 0°/90°/180°, 实际角度测量           │
  │ ToF            │ 已知距离物体, 校验精度               │
  │ IMU            │ 静止状态, 校验零偏                   │
  │ BLE            │ 手机扫描发现, 连接, 数据收发         │
  │ WiFi           │ 连接AP, ping测试, 吞吐量            │
  │ 充电触点       │ 接触充电座, 检测充电电流             │
  │ LED            │ 全亮红/绿/蓝, 目视确认               │
  └────────────────┴──────────────────────────────────────┘

自动化:
  - 串口脚本控制测试流程
  - Python pytest 采集结果
  - 每次固件构建后自动运行
```

### 2.3 协议一致性测试

```
目标: 确保固件、主脑、移动端的BCP实现完全一致

测试向量 (固定输入→期望输出):

  向量1: 心跳帧
    输入: Magic=0xCB, Ver=0x01, SeqNo=1, CmdCount=1, Cmd=HEARTBEAT(0x0001), Payload=[0x01,0x64,0xC8,0x0A,0x00]
    期望: CRC16=0xXXXX, TotalLen=0x0F

  向量2: 集束帧(2条指令)
    输入: MOVE(前半速) + LED(红色流水)
    期望: TotalLen=0x10, CmdCount=2

  向量3: 最大帧
    输入: TotalLen=1024, Payload填满
    期望: 正确解析, 无溢出

  向量4: 边界值
    输入: CmdCount=0 (空帧)
    期望: 正确处理, 返回0条指令

  向量5: 异常帧
    输入: Magic错误
    期望: 返回-2

共享测试套件:
  - 三个平台(C/Rust/Kotlin)各实现同一套测试向量
  - CI自动运行, 任一平台失败则阻断合并
```

## 3. 主脑测试 (Rust)

### 3.1 单元测试

```
框架: Rust内置 #[test] + mockall

测试目标:
  bcp_codec::encode() / decode()  — BCP编解码
  scheduler::assign_task()        — 任务分配算法
  device_manager::heartbeat()     — 心跳处理
  auth::verify_token()            — JWT验证
  conn_manager::session_sticky()  — 会话粘滞

示例:
  #[test]
  fn test_assign_task_prefer_idle_robot() {
      let robots = vec![
          Robot { id: 1, status: Online, active_tasks: 2, battery: 80, .. },
          Robot { id: 2, status: Online, active_tasks: 0, battery: 60, .. },
      ];
      let task = Task { requirements: vec![], .. };
      let assigned = scheduler::assign_task(&task, &robots);
      assert_eq!(assigned, Some(2)); // 优先空闲机器人
  }

  #[test]
  fn test_assign_task_skip_low_battery() {
      let robots = vec![
          Robot { id: 1, status: Online, battery: 10, .. },
      ];
      let task = Task { .. };
      let assigned = scheduler::assign_task(&task, &robots);
      assert_eq!(assigned, None); // 电量不足, 不分配
  }
```

### 3.2 集成测试

```
框架: tokio::test + testcontainers

测试环境:
  - NATS: testcontainers启动临时实例
  - Redis: testcontainers启动临时实例
  - PostgreSQL: testcontainers启动临时实例

测试场景:
  端到端注册:
    1. 启动Gateway + DeviceManager
    2. 模拟机器人WebSocket连接
    3. 发送REGISTER指令
    4. 验证数据库中机器人记录
    5. 验证返回REG_ACK含短ID

  心跳超时:
    1. 注册机器人, 短ID=1
    2. 发送心跳
    3. 等待15s不发心跳
    4. 验证机器人状态变为Offline

  任务分配:
    1. 注册2个机器人
    2. 创建任务
    3. 验证任务分配给最优机器人
    4. 验证NATS消息到达正确机器人

  模式切换:
    1. 机器人直连
    2. 断开WebSocket
    3. 模拟BLE桥接连接
    4. 验证指令通过BLE通道到达
```

### 3.3 负载测试

```
工具: k6 (JavaScript负载测试)

脚本示例:
  import ws from 'k6/ws';
  import { check } from 'k6';

  export const options = {
    stages: [
      { duration: '30s', target: 100 },  // 爬升到100连接
      { duration: '1m',  target: 500 },  // 爬升到500
      { duration: '2m',  target: 1000 }, // 维持1000
      { duration: '30s', target: 0 },    // 降为0
    ],
    thresholds: {
      ws_connecting: ['p(95)<500'],    // 95%连接 <500ms
      ws_msgs_received: ['count>0'],   // 能收到消息
    },
  };

  export default function () {
    const url = `ws://gateway:8080/ws?token=${__ENV.TOKEN}`;
    const res = ws.connect(url, function (socket) {
      socket.on('open', () => {
        // 发送BCP注册帧
        socket.sendBinary(registerFrame());
        // 模拟心跳
        socket.setInterval(() => {
          socket.sendBinary(heartbeatFrame());
        }, 5000);
      });
      socket.on('message', (data) => {
        check(data, { 'BCP response': (d) => d.length > 0 });
      });
      socket.setTimeout(() => socket.close(), 120000);
    });
    check(res, { 'connected': (r) => r && r.status === 101 });
  }

运行: k6 run --vus 100 --duration 5m load_test.js

测试指标:
  - 连接建立延迟 p50/p95/p99
  - 消息往返延迟 p50/p95/p99
  - 连接成功率
  - 消息丢失率
  - Gateway CPU/内存使用
  - NATS消息积压
```

## 4. 移动端测试

### 4.1 单元测试

```
iOS: XCTest
Android: JUnit + MockK

测试目标:
  BcpCodec.encode / decode — BCP编解码
  BleManager — 连接/断连/重连逻辑 (mock)
  WsManager — WebSocket消息处理 (mock)
  LocalNlu — 意图识别
  BehaviorEngine — 行为状态机
  CacheManager — 缓存策略
```

### 4.2 BLE集成测试

```
需要: 真实手机 + ESP32开发板

测试矩阵:
  ┌──────────────┬────────┬────────┐
  │ 测试项       │ iOS    │ Android│
  ├──────────────┼────────┼────────┤
  │ 扫描发现     │ ✓      │ ✓      │
  │ 连接         │ ✓      │ ✓      │
  │ MTU协商      │ ✓      │ ✓      │
  │ 数据收发     │ ✓      │ ✓      │
  │ 分片传输     │ ✓      │ ✓      │
  │ 后台保活     │ ✓      │ ✓      │
  │ 断连重连     │ ✓      │ ✓      │
  │ 多设备连接   │ ✓      │ ✓      │
  └──────────────┴────────┴────────┘

自动化:
  - iOS: XCUITest + 真机
  - Android: Espresso + 真机
  - CI: 每晚运行 (需要设备农场)
```

## 5. 端到端测试

```
场景: 完整用户流程

E2E-01: 首次配对
  1. 手机APP扫描 → 发现机器人
  2. BLE配对 → 连接成功
  3. 读取设备信息 → 显示正确
  4. 发送控制指令 → 机器人响应

E2E-02: 桥接模式对话
  1. 手机BLE连接机器人
  2. 手机WS连接主脑
  3. 用户按住说话 → 音频流发送
  4. 主脑ASR → 回复
  5. 音频流回传 → 机器人播放

E2E-03: 多机任务分配
  1. 2个机器人注册
  2. 创建任务A → 分配给机器人1
  3. 创建任务B → 分配给机器人2
  4. 机器人1完成 → 状态更新
  5. 创建任务C → 分配给机器人1 (空闲)

E2E-04: 断网降级
  1. 机器人直连主脑
  2. 断开主脑网络
  3. 等待5s → 自动切BLE桥接
  4. 手机本地处理语音
  5. 恢复网络 → 自动切回直连

E2E-05: 低电量充电
  1. 机器人电量降至15%
  2. 主脑下发充电任务
  3. 机器人扫描信标 → 导航到充电座
  4. 对接成功 → 充电中
  5. 电量80% → 离开充电座
```

## 6. CI/CD集成

```
┌─────────────────────────────────────────────────────────┐
│  GitHub Actions / GitLab CI                             │
│                                                         │
│  固件 CI:                                               │
│    on: push / PR                                        │
│    jobs:                                                │
│      - idf_build (ESP-IDF编译)                          │
│      - unit_test (运行单元测试)                         │
│      - protocol_conformance (协议一致性)                │
│      - hil_test (HIL测试, 需要硬件runner)              │
│    artifacts: firmware.bin                              │
│                                                         │
│  主脑 CI:                                               │
│    on: push / PR                                        │
│    jobs:                                                │
│      - cargo_build                                      │
│      - cargo_test (单元测试)                            │
│      - integration_test (testcontainers)                │
│      - cargo_clippy (lint)                              │
│      - cargo_fmt (格式检查)                             │
│    artifacts: docker image                              │
│                                                         │
│  移动端 CI:                                             │
│    on: push / PR                                        │
│    jobs:                                                │
│      - xcodebuild (iOS)                                │
│      - gradle_build (Android)                           │
│      - unit_test                                        │
│      - lint                                             │
│    artifacts: .ipa / .apk                              │
│                                                         │
│  部署:                                                  │
│    on: tag (v*)                                         │
│    jobs:                                                │
│      - 固件OTA发布 (主脑API触发)                       │
│      - 主脑Docker推送 + 部署                            │
│      - 移动端上传 TestFlight / Play Console             │
└─────────────────────────────────────────────────────────┘
```
