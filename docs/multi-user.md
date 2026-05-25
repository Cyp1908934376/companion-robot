# 多用户与家庭共享

## 1. 数据模型

### 1.1 实体关系

```
┌──────────┐     ┌──────────────┐     ┌──────────┐
│  Family   │────►│ FamilyMember │◄────│   User   │
│  家庭     │     │  家庭成员    │     │  用户    │
└──────────┘     └──────────────┘     └──────────┘
     │                                      │
     │                                      │
     ▼                                      ▼
┌──────────┐     ┌──────────────┐     ┌──────────┐
│  Robot   │────►│RobotAccess   │◄────│  Role    │
│  机器人   │     │ 访问权限     │     │  角色    │
└──────────┘     └──────────────┘     └──────────┘
```

### 1.2 数据库Schema

```sql
-- 家庭
CREATE TABLE families (
    id          BIGSERIAL PRIMARY KEY,
    name        VARCHAR(64) NOT NULL,
    invite_code VARCHAR(16) UNIQUE,      -- 邀请码, 用于加入家庭
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- 用户
CREATE TABLE users (
    id          BIGSERIAL PRIMARY KEY,
    phone       VARCHAR(20) UNIQUE,      -- 手机号登录
    nickname    VARCHAR(64),
    avatar_url  VARCHAR(256),
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- 家庭成员
CREATE TABLE family_members (
    family_id   BIGINT REFERENCES families(id),
    user_id     BIGINT REFERENCES users(id),
    role        SMALLINT NOT NULL,       -- 0=Owner, 1=Admin, 2=Member, 3=Guest
    joined_at   TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (family_id, user_id)
);

-- 机器人
CREATE TABLE robots (
    id          BIGSERIAL PRIMARY KEY,
    family_id   BIGINT REFERENCES families(id),  -- 所属家庭
    machine_id  BYTEA UNIQUE NOT NULL,
    short_id    SMALLINT,
    name        VARCHAR(64),
    capabilities JSONB,
    firmware_ver VARCHAR(32),
    status      SMALLINT DEFAULT 0,
    last_heartbeat TIMESTAMPTZ,
    battery     SMALLINT,
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- 机器人访问权限 (细粒度)
CREATE TABLE robot_access (
    user_id     BIGINT REFERENCES users(id),
    robot_id    BIGINT REFERENCES robots(id),
    permissions INT NOT NULL DEFAULT 0,  -- 位掩码
    granted_by  BIGINT REFERENCES users(id),
    granted_at  TIMESTAMPTZ DEFAULT NOW(),
    expires_at  TIMESTAMPTZ,             -- 临时权限过期时间
    PRIMARY KEY (user_id, robot_id)
);

-- 权限位掩码
-- bit0: 查看状态
-- bit1: 查看传感器
-- bit2: 控制运动
-- bit3: 控制表达
-- bit4: 对话
-- bit5: 任务管理
-- bit6: 配置修改
-- bit7: OTA更新
-- bit8: 访客临时权限
```

## 2. 角色与权限

### 2.1 家庭角色

```
Owner (家庭创建者):
  - 完全控制所有机器人
  - 管理家庭成员 (邀请/移除)
  - 分配权限
  - 删除家庭
  - 每个家庭只有一个Owner

Admin (管理员):
  - 控制所有机器人
  - 邀请成员 (不能移除Owner)
  - 分配权限给Member/Guest
  - 每个家庭可有多个Admin

Member (普通成员):
  - 控制分配给自己的机器人
  - 查看家庭内所有机器人状态
  - 不能管理成员或权限

Guest (访客):
  - 临时访问, 有过期时间
  - 仅能控制被明确授权的机器人
  - 权限受限 (默认只有查看+对话)
```

### 2.2 权限矩阵

```
操作                  Owner  Admin  Member  Guest
──────────────────────────────────────────────────
查看机器人列表         ✓      ✓      ✓      ✓
查看机器人状态         ✓      ✓      ✓      ✓
查看传感器数据         ✓      ✓      限定   ✗
控制运动              ✓      ✓      限定   ✗
控制表达/LED          ✓      ✓      限定   限定
对话                  ✓      ✓      ✓      ✓
创建任务              ✓      ✓      限定   ✗
管理成员              ✓      ✓      ✗      ✗
分配权限              ✓      ✓      ✗      ✗
修改机器人配置         ✓      ✗      ✗      ✗
OTA更新               ✓      ✗      ✗      ✗
删除机器人             ✓      ✗      ✗      ✗

限定: 仅限被分配权限的机器人
```

## 3. 并发控制

### 3.1 多用户同时操作

```
场景: 爸爸在控制机器人前进, 妈妈同时想让机器人说话

方案: 无锁协作, 最后写入胜出

  - 运动控制: 多用户可同时发送, 机器人端合并 (取最新)
  - 表达控制: 同上
  - 对话: 并行, 无冲突
  - 任务: 需要获取操作锁

操作锁 (仅任务管理):
  1. 用户A请求创建任务 → 检查锁
  2. 无锁 → 获取锁 (30s TTL) → 创建任务 → 释放锁
  3. 有锁 → 返回"机器人忙碌中" → 等待或取消
```

### 3.2 用户识别

```
机器人如何知道是哪个用户在交互?

方式1: 语音识别 (Phase 2+)
  - 声纹识别: 预注册家庭成员声纹
  - 准确率: ~90% (安静环境)
  - 局限: 需要预先注册, 新用户需要适应期

方式2: 手机绑定 (Phase 1)
  - 手机APP连接时携带user_id
  - 机器人根据连接来源识别用户
  - 局限: 不同用户用同一手机时无法区分

方式3: 主动声明 (Phase 1)
  - 用户通过APP选择"我现在在跟机器人互动"
  - 机器人标记当前交互用户
  - 局限: 需要用户主动操作

推荐: Phase 1用方式2+3组合, Phase 2增加方式1
```

## 4. 访客权限

### 4.1 临时访问流程

```
场景: 朋友来家里, 想跟机器人玩

流程:
  1. 家庭成员打开APP → 选择机器人 → "分享给访客"
  2. 生成临时邀请 (二维码/链接)
     - 有效期: 1小时/4小时/当天/自定义
     - 权限: 查看+对话 (可自定义)
  3. 访客扫码/点击链接 → 自动注册为Guest
  4. 访客可在有效期内控制机器人
  5. 过期后自动失去权限

数据隔离:
  - 访客的对话记录不显示给其他用户
  - 访客看不到历史对话
  - 访客操作记录可被Owner/Admin查看 (审计)
```

### 4.2 邀请码机制

```
家庭邀请码:
  - 8位字母数字, 如 "AB12CD34"
  - 有效期: 7天
  - 使用次数: 无限
  - 加入后角色: Member (可由Admin调整)

机器人临时码:
  - 6位数字, 如 "123456"
  - 有效期: 1-24小时
  - 使用次数: 1次
  - 权限: 预设 (默认查看+对话)
```

## 5. 儿童安全

### 5.1 内容过滤

```
语音输入过滤:
  - 敏感词库 (暴力/色情/政治)
  - 本地匹配, 不上传到服务器
  - 匹配到 → 固定回复 "我不太明白, 换个话题吧"
  - 日志标记, 家长可查看

AI回复过滤:
  - 主脑生成回复后, 过滤敏感内容
  - 过滤规则: 正则匹配 + 关键词列表
  - 被过滤 → 替换为安全回复

视觉内容:
  - 图像不存储原始数据
  - 人脸检测结果脱敏 (只保留"检测到人")
  - 不进行人脸识别 (隐私保护)
```

### 5.2 使用时间控制

```
家长可设置:
  - 每日使用时长限制 (如: 2小时/天)
  - 允许使用时段 (如: 8:00-21:00)
  - 休息提醒 (每30分钟提醒休息)

触发:
  - 达到时长限制 → 机器人进入休息模式
  - 表情显示"我要休息了, 明天见"
  - 禁止对话和控制 (除了家长解除)
  - 家长可通过APP临时延长

休息模式:
  - 关闭摄像头、麦克风
  - LED低亮度呼吸
  - 仅保留时钟显示
  - 家长可远程唤醒
```

### 5.3 交互监控

```
家长可查看:
  - 今日对话摘要 (不显示具体内容, 只显示主题)
  - 互动时长统计
  - 机器人情绪状态 (开心/无聊/困倦)
  - 异常事件 (长时间无人互动/频繁触碰)

隐私平衡:
  - 不记录对话原文 (仅关键词统计)
  - 不存储图像
  - 传感器数据仅保留聚合值
  - 家长可选择"完全不监控"模式
```

## 6. 数据隔离

### 6.1 存储隔离

```
PostgreSQL:
  所有查询自动添加 family_id 过滤
  -- RLS (Row Level Security) 策略
  ALTER TABLE robots ENABLE ROW LEVEL SECURITY;
  CREATE POLICY family_isolation ON robots
    USING (family_id = current_setting('app.family_id')::bigint);

  跨家庭访问: 禁止 (数据库层面强制)

TimescaleDB:
  sensor_data 表增加 family_id 列
  查询时自动过滤

Redis:
  Key前缀: family:{family_id}:robot:{robot_id}:*
  查询时使用前缀匹配
```

### 6.2 API隔离

```
所有API请求自动注入family_id:

  middleware:
    1. 解析JWT → 获取user_id
    2. 查询family_members → 获取family_id
    3. 注入请求上下文: ctx.family_id = family_id
    4. 所有数据库查询使用ctx.family_id

  跨家庭访问:
    - 直接返回403 Forbidden
    - 记录安全审计日志
```

### 6.3 通信隔离

```
NATS Subject隔离:
  robot.{family_id}.{short_id}.cmd
  robot.{family_id}.{short_id}.event

  订阅时只能订阅自己家庭的subject

WebSocket:
  连接时验证用户所属家庭
  机器人只能收到自己家庭的指令
```

## 7. 用户画像

### 7.1 个性化

```
每个用户可设置:
  - 昵称 (机器人称呼用户的名字)
  - 偏好语言
  - 喜欢的机器人表情风格
  - 对话偏好 (健谈/安静)

机器人记住:
  - 每个用户的对话历史 (本地或主脑)
  - 用户常问的问题
  - 用户的作息时间
  - 用户的情绪倾向

个性化回复:
  "早上好, 小明" (而不是通用的"早上好")
  "你上次问的天气, 今天转晴了" (记住上下文)
```

### 7.2 识别与切换

```
用户切换流程:
  1. 用户A通过APP控制机器人
  2. 用户B的手机BLE连接机器人
  3. 机器人收到新user_id
  4. 机器人切换上下文: "嗨, 小红, 有什么事?"
  5. 用户A的APP显示"机器人正在和小红互动"

冲突处理:
  - 两个用户同时发语音 → 机器人选择最近连接的用户
  - 两个用户同时控运动 → 取最新的指令
  - 任务操作 → 需要获取锁
```
