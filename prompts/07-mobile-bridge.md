# 移动端桥接与边缘计算实现提示词

## 角色

你是一个移动端开发工程师。请根据以下架构文档实现桥接模式和边缘计算功能。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/mobile-sdk.md` §5-6 — 双通道选择与边缘计算
- `docs/protocol.md` — BCP协议

## 实现要求

### 1. 双通道桥接

```kotlin
// Android
class BridgeManager(
    private val bleTransport: BcpBleTransport,
    private val wsClient: WsClient,
    private val localProcessor: LocalProcessor,
) {
    private val mode = MutableStateFlow(BridgeMode.BRIDGE)

    // 数据流: 机器人→手机→主脑
    fun startBridge() {
        // BLE上行 → WS转发
        scope.launch {
            bleTransport.receive().collect { frame ->
                when {
                    // 本地处理的指令 (短延迟)
                    isLocalCommand(frame) -> localProcessor.process(frame)
                    // 转发到主脑
                    else -> wsClient.send(frame)
                }
            }
        }

        // WS下行 → BLE转发
        scope.launch {
            wsClient.receive().collect { frame ->
                when {
                    // 主脑发来的本地指令
                    isLocalInstruction(frame) -> localProcessor.execute(frame)
                    // 转发到机器人
                    else -> bleTransport.send(frame)
                }
            }
        }
    }

    private fun isLocalCommand(frame: BcpFrame): Boolean {
        return frame.commands.any { cmd ->
            cmd is Command.Move || cmd is Command.Stop || cmd is Command.LedSet
            // 运动控制、急停、LED本地处理
        }
    }
}
```

### 2. 本地语音处理

```kotlin
// Android
class LocalAsr(private val context: Context) {
    private val recognizer = SpeechRecognizer.createSpeechRecognizer(context)

    suspend fun recognize(audioData: ByteArray): String? {
        return suspendCancellableCoroutine { cont ->
            val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, false)
            }

            recognizer.setRecognitionListener(object : RecognitionListener {
                override fun onResults(results: Bundle?) {
                    val matches = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
                    cont.resume(matches?.firstOrNull())
                }
                override fun onError(error: Int) {
                    cont.resume(null)
                }
                // ... 其他回调
            })

            recognizer.startListening(intent)
        }
    }
}

// iOS
class LocalAsr {
    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "zh-CN"))

    func recognize(audioBuffer: AVAudioPCMBuffer) async -> String? {
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.append(audioBuffer)

        do {
            let result = try await recognizer?.recognitionTask(with: request)
            return result?.bestTranscription.formattedString
        } catch {
            return nil
        }
    }
}
```

### 3. 本地NLU (关键词匹配)

```kotlin
class LocalNlu {
    private val intentMap = mapOf(
        "time" to listOf("几点", "时间", "现在几点"),
        "battery" to listOf("电量", "还有多少电", "充电"),
        "move_forward" to listOf("前进", "向前", "往前走"),
        "move_backward" to listOf("后退", "向后", "往后"),
        "stop" to listOf("停下", "停止", "别动"),
        "greet" to listOf("你好", "嗨", "早上好", "晚上好"),
        "dance" to listOf("跳舞", "来一个", "表演"),
        "volume_up" to listOf("大声点", "音量大", "太小声"),
        "volume_down" to listOf("小声点", "音量小", "太吵"),
    )

    fun recognize(text: String): NluResult {
        for ((intent, keywords) in intentMap) {
            if (keywords.any { text.contains(it) }) {
                return NluResult(intent, confidence = 0.9f)
            }
        }
        return NluResult("unknown", confidence = 0.0f)
    }
}
```

### 4. 本地行为引擎

```kotlin
class BehaviorEngine {
    private var state = BehaviorState.IDLE
    private val transitions = mapOf(
        BehaviorState.IDLE to mapOf(
            Event.PersonDetected to BehaviorState.GREETING,
            Event.LowBattery to BehaviorState.CHARGING,
            Event.VoiceCommand("follow") to BehaviorState.FOLLOWING,
        ),
        BehaviorState.GREETING to mapOf(
            Event.Timeout(5000) to BehaviorState.IDLE,
        ),
        BehaviorState.FOLLOWING to mapOf(
            Event.VoiceCommand("stop") to BehaviorState.IDLE,
            Event.LostTarget to BehaviorState.IDLE,
        ),
        BehaviorState.CHARGING to mapOf(
            Event.BatteryFull to BehaviorState.IDLE,
        ),
    )

    fun onEvent(event: Event): List<Command> {
        val nextState = transitions[state]?.get(event)
        if (nextState != null) {
            val actions = getEntryActions(nextState)
            state = nextState
            return actions
        }
        return emptyList()
    }

    private fun getEntryActions(state: BehaviorState): List<Command> {
        return when (state) {
            BehaviorState.GREETING -> listOf(
                Command.FaceExpression(1), // 开心
                Command.Speak(volume = 80, text = "你好呀!"),
            )
            BehaviorState.CHARGING -> listOf(
                Command.LedPattern(pattern = 0, speed = 0x20, r = 0, g = 255, b = 0), // 绿色呼吸
            )
            else -> emptyList()
        }
    }
}
```

### 5. 离线任务队列

```kotlin
class OfflineTaskQueue(private val db: AppDatabase) {
    suspend fun enqueue(task: OfflineTask) {
        db.offlineTaskDao().insert(task.copy(status = TaskStatus.PENDING))
    }

    suspend fun executeNext(): OfflineTask? {
        val task = db.offlineTaskDao().getNextPending() ?: return null
        db.offlineTaskDao().updateStatus(task.id, TaskStatus.RUNNING)
        return task
    }

    suspend fun complete(taskId: Long) {
        db.offlineTaskDao().updateStatus(taskId, TaskStatus.COMPLETED)
    }

    // 上线后同步到主脑
    suspend fun syncToMainBrain(wsClient: WsClient) {
        val completed = db.offlineTaskDao().getCompleted()
        for (task in completed) {
            wsClient.send(task.toBcpFrame())
            db.offlineTaskDao().updateStatus(task.id, TaskStatus.SYNCED)
        }
    }
}
```

## 约束

- 本地ASR延迟: <500ms
- 本地NLU: 200+关键词
- 行为引擎: 状态机驱动
- 离线队列: SQLite持久化
- 上线同步: 批量上报
- 资源管控: 按CPU/内存/电量动态降级
