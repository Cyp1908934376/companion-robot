import Foundation
import CoreBluetooth
import Combine

/// BLE Manager — scan, connect, MTU negotiation, GATT operations.
///
/// BCP Service:
///   UUID 0xCB00: BCP Service
///     - 0xCB01: BCP_TX (Write) — mobile → robot
///     - 0xCB02: BCP_RX (Notify) — robot → mobile
///     - 0xCB03: BCP_CONTROL (Read/Write) — connection params
///     - 0xCB04: DEVICE_INFO (Read) — robot metadata

struct BleConstants {
    static let serviceUUID = CBUUID(string: "CB00")
    static let charBcpTx = CBUUID(string: "CB01")
    static let charBcpRx = CBUUID(string: "CB02")
    static let charBcpControl = CBUUID(string: "CB03")
    static let charDeviceInfo = CBUUID(string: "CB04")
    static let charOtaData = CBUUID(string: "CB05")
    static let deviceNamePrefix = "CompanionBot"
    static let targetMtu = 247
}

/// Events from the BLE layer.
enum BleEvent {
    case deviceDiscovered(DiscoveredDevice)
    case connectionStateChanged(deviceId: String, state: CBPeripheralState)
    case mtuNegotiated(mtu: Int)
    case servicesDiscovered
    case frameReceived(Data)
    case error(String)
}

class BleManager: NSObject, ObservableObject {
    @Published var discoveredDevices: [DiscoveredDevice] = []
    @Published var connectedPeripheral: CBPeripheral?
    @Published var isScanning = false

    let events = PassthroughSubject<BleEvent, Never>()

    private var centralManager: CBCentralManager!
    private var txCharacteristic: CBCharacteristic?
    private var negotiatedMtu: Int = 23

    // Fragment reassembly
    private var fragmentBuffer = Data()
    private var expectedTotalLen = 0
    private var fragmentTimestamp: Date?

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: .main)
    }

    // MARK: - Scanning

    func startScan() {
        guard centralManager.state == .poweredOn else {
            events.send(.error("Bluetooth not powered on"))
            return
        }

        isScanning = true
        discoveredDevices.removeAll()
        centralManager.scanForPeripherals(
            withServices: [BleConstants.serviceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    func stopScan() {
        isScanning = false
        centralManager.stopScan()
    }

    // MARK: - Connection

    func connect(to peripheral: CBPeripheral) {
        centralManager.connect(peripheral, options: nil)
    }

    func disconnect() {
        if let peripheral = connectedPeripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        connectedPeripheral = nil
        txCharacteristic = nil
    }

    // MARK: - Data transfer

    func sendBcpFrame(_ frame: BcpCodec.BcpFrame) -> Bool {
        guard let tx = txCharacteristic else { return false }

        let data = BcpCodec.encode(frame: frame)
        let effectiveMtu = negotiatedMtu - 3

        if data.count <= effectiveMtu {
            // Single write
            connectedPeripheral?.writeValue(data, for: tx, type: .withResponse)
            return true
        }

        // Fragment and send
        var offset = 0
        while offset < data.count {
            let chunkSize = min(effectiveMtu, data.count - offset)
            var chunk = Data(capacity: chunkSize + 2)

            // Fragment header: [totalLen_hi, totalLen_lo] + data
            chunk.append(UInt8(data.count >> 8))
            chunk.append(UInt8(data.count & 0xFF))
            chunk.append(data.subdata(in: offset..<offset + chunkSize))

            connectedPeripheral?.writeValue(chunk, for: tx, type: .withResponse)
            offset += chunkSize
        }

        return true
    }

    // MARK: - Fragment handling

    private func handleReceivedData(_ data: Data) {
        guard data.count >= 2 else { return }

        // Check if fragment (first 2 bytes = total length)
        let totalLen = (Int(data[0]) << 8) | Int(data[1])

        if totalLen > 0 && totalLen <= BcpCodec.maxFrameLen {
            // Fragment mode
            if fragmentBuffer.isEmpty {
                expectedTotalLen = totalLen
                fragmentTimestamp = Date()
            }
            fragmentBuffer.append(data.dropFirst(2))
        } else {
            // Complete frame
            fragmentBuffer = Data(data)
            expectedTotalLen = data.count
        }

        // Check completion
        if !fragmentBuffer.isEmpty && fragmentBuffer.count >= expectedTotalLen {
            let completeFrame = fragmentBuffer.prefix(expectedTotalLen)
            events.send(.frameReceived(Data(completeFrame)))
            fragmentBuffer.removeAll()
            expectedTotalLen = 0
        }

        // Fragment timeout (2 seconds)
        if !fragmentBuffer.isEmpty,
           let ts = fragmentTimestamp,
           Date().timeIntervalSince(ts) > 2.0 {
            fragmentBuffer.removeAll()
            expectedTotalLen = 0
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BleManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            break
        case .poweredOff:
            events.send(.error("Bluetooth is powered off"))
        case .unsupported:
            events.send(.error("BLE not supported on this device"))
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let name = peripheral.name ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? "Unknown"
        let device = DiscoveredDevice(
            name: name,
            peripheralId: peripheral.identifier,
            rssi: RSSI.intValue
        )

        if !discoveredDevices.contains(where: { $0.peripheralId == peripheral.identifier }) {
            discoveredDevices.append(device)
            events.send(.deviceDiscovered(device))
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedPeripheral = peripheral
        peripheral.delegate = self
        peripheral.discoverServices([BleConstants.serviceUUID])
        events.send(.connectionStateChanged(deviceId: peripheral.identifier.uuidString, state: .connected))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connectedPeripheral = nil
        txCharacteristic = nil
        events.send(.connectionStateChanged(deviceId: peripheral.identifier.uuidString, state: .disconnected))
    }
}

// MARK: - CBPeripheralDelegate

extension BleManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }

        for service in services where service.uuid == BleConstants.serviceUUID {
            peripheral.discoverCharacteristics([
                BleConstants.charBcpTx,
                BleConstants.charBcpRx,
                BleConstants.charBcpControl,
                BleConstants.charDeviceInfo
            ], for: service)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let characteristics = service.characteristics else { return }

        for characteristic in characteristics {
            switch characteristic.uuid {
            case BleConstants.charBcpTx:
                txCharacteristic = characteristic
            case BleConstants.charBcpRx:
                peripheral.setNotifyValue(true, for: characteristic)
            case BleConstants.charDeviceInfo:
                peripheral.readValue(for: characteristic)
            default:
                break
            }
        }

        events.send(.servicesDiscovered)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard let data = characteristic.value, error == nil else { return }

        switch characteristic.uuid {
        case BleConstants.charBcpRx:
            handleReceivedData(data)
        case BleConstants.charDeviceInfo:
            events.send(.frameReceived(data))
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didWriteValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            events.send(.error("Write failed: \(error.localizedDescription)"))
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error = error {
            events.send(.error("Notification setup failed: \(error.localizedDescription)"))
        }
    }
}
