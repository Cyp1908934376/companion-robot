# 移动端BLE通信层实现提示词

## 角色

你是一个移动端开发工程师。请根据以下架构文档实现BLE通信层。

## 参考文档

请先阅读以下文档获取完整上下文：
- `docs/mobile-sdk.md` — 移动端SDK架构
- `docs/protocol.md` — BCP协议规范（BLE GATT服务定义 §4.3）

## 实现要求

### Android (Kotlin)

```
feature/ble/
├── src/main/java/com/companion/ble/
│   ├── BleManager.kt          — BLE连接管理
│   ├── BleScanner.kt          — 设备扫描
│   ├── BcpBleTransport.kt     — BCP over BLE
│   ├── BleGattService.kt      — GATT服务交互
│   ├── BleFragmenter.kt       — 帧分片
│   └── BleReassembler.kt      — 帧重组
```

### iOS (Swift)

```
CompanionBLE/
├── Sources/
│   ├── BleManager.swift
│   ├── BleScanner.swift
│   ├── BcpBleTransport.swift
│   ├── BleGattService.swift
│   └── BleFragmenter.swift
```

## 关键实现

### 1. 扫描与连接

```kotlin
// Android
class BleScanner(private val context: Context) {
    private val scanner = BluetoothLeScanner.getScanner()

    fun scan(): Flow<BleDevice> = callbackFlow {
        val callback = object : ScanCallback() {
            override fun onScanResult(type: Int, result: ScanResult) {
                // 过滤: Service UUID = 0xCB00
                if (result.scanRecord?.serviceUuids?.contains(PARCEL_UUID_0xCB00) == true) {
                    trySend(BleDevice(result.device, result.rssi))
                }
            }
        }

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()

        scanner.startScan(listOf(filter), settings, callback)
        awaitClose { scanner.stopScan(callback) }
    }
}

// iOS
class BleScanner: NSObject, CBCentralManagerDelegate {
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // 过滤: Service UUID = 0xCB00
        if let services = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] {
            if services.contains(CBUUID(string: "CB00")) {
                discoveredDevices.send(BleDevice(peripheral: peripheral, rssi: RSSI.intValue))
            }
        }
    }
}
```

### 2. MTU协商

```kotlin
// Android
fun requestMtu(device: BluetoothDevice, desiredMtu: Int = 247): Single<Int> {
    return device.requestMtu(desiredMtu)
        .map { it }
        .timeout(5, TimeUnit.SECONDS)
}

// iOS — MTU由系统自动协商，无需手动处理
// 但可以通过 peripheral.maximumWriteValueLength(for: .withoutResponse) 获取
```

### 3. GATT服务交互

```kotlin
// Android
class BleGattService(private val gatt: BluetoothGatt) {
    private val service = gatt.getService(SERVICE_UUID)
    private val txChar = service.getCharacteristic(CHAR_BCP_TX)      // 0xCB01 Write
    private val rxChar = service.getCharacteristic(CHAR_BCP_RX)      // 0xCB02 Notify

    fun enableNotifications() {
        gatt.setCharacteristicNotification(rxChar, true)
        val descriptor = rxChar.getDescriptor(CCCD_UUID)
        descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        gatt.writeDescriptor(descriptor)
    }

    fun write(data: ByteArray) {
        txChar.value = data
        txChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        gatt.writeCharacteristic(txChar)
    }

    // 接收通知
    fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        if (characteristic.uuid == CHAR_BCP_RX) {
            onBcpFrameReceived(characteristic.value)
        }
    }
}
```

### 4. 帧分片/重组

```kotlin
// Android
class BleFragmenter(private val mtu: Int) {
    fun fragment(frame: ByteArray): List<ByteArray> {
        if (frame.size <= mtu - 3) return listOf(frame)

        val maxDataPerFragment = mtu - 4 // 1字节分片头 + 3字节ATT开销
        val fragments = mutableListOf<ByteArray>()
        var offset = 0
        var seqNo = 0

        while (offset < frame.size) {
            val end = minOf(offset + maxDataPerFragment, frame.size)
            val isLast = end == frame.size

            val header = ((if (isLast) 0x80 else 0x00) or (seqNo and 0x7F)).toByte()
            val fragment = ByteArray(1 + (end - offset))
            fragment[0] = header
            frame.copyInto(fragment, 1, offset, end)

            fragments.add(fragment)
            offset = end
            seqNo++
        }

        return fragments
    }
}

class BleReassembler {
    private val buffer = ByteArrayOutputStream()
    private var expectedSeqNo = 0
    private var timeoutJob: Job? = null

    fun feed(fragment: ByteArray): ByteArray? {
        val header = fragment[0].toInt() and 0xFF
        val isLast = (header and 0x80) != 0
        val seqNo = header and 0x7F

        if (seqNo != expectedSeqNo) {
            reset()
            return null
        }

        buffer.write(fragment, 1, fragment.size - 1)
        expectedSeqNo++

        // 2s超时重置
        timeoutJob?.cancel()
        timeoutJob = scope.launch {
            delay(2000)
            reset()
        }

        if (isLast) {
            val result = buffer.toByteArray()
            reset()
            return result
        }

        return null
    }
}
```

### 5. BCP收发

```kotlin
// Android
class BcpBleTransport(private val gattService: BleGattService) {
    private val fragmenter = BleFragmenter(mtu = 247)
    private val reassembler = BleReassembler()
    private val incoming = MutableSharedFlow<BcpFrame>()

    suspend fun send(frame: BcpFrame) {
        val encoded = BcpCodec.encode(frame)
        val fragments = fragmenter.fragment(encoded)
        for (fragment in fragments) {
            gattService.write(fragment)
            delay(10) // BLE写入间隔
        }
    }

    fun receive(): Flow<BcpFrame> = incoming

    fun onBcpFrameReceived(data: ByteArray) {
        val frame = BcpCodec.decode(data)
        incoming.tryEmit(frame)
    }

    fun onBleFragmentReceived(fragment: ByteArray) {
        val completeFrame = reassembler.feed(fragment) ?: return
        onBcpFrameReceived(completeFrame)
    }
}
```

## 约束

- MTU: 247字节
- 分片超时: 2s
- BLE写入间隔: 10ms
- 后台保活: 3s空帧心跳
- 断连重连: 指数退避 1s→8s
- Service UUID: 0xCB00
- TX Char: 0xCB01 (Write)
- RX Char: 0xCB02 (Notify)
