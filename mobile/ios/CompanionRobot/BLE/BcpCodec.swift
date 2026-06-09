import Foundation

/// BCP (Bundle Command Protocol) codec — Swift implementation.
///
/// Wire format (little-endian):
///   Header:  Magic(1B) + Version(1B) + TotalLen(2B LE) + SeqNo(2B LE) + CmdCount(1B) + Reserved(1B)
///   Commands: CmdID(2B LE) + PayloadLen(1B) + Payload
///   CRC: CRC-16/CCITT over bytes 0..TotalLen-2

struct BcpCodec {
    static let magic: UInt8 = 0xCB
    static let version: UInt8 = 0x01
    static let headerLen = 8
    static let crcLen = 2
    static let maxFrameLen = 1024
    static let maxCommands = 32
    static let maxPayloadLen = 255

    // Command IDs
    static let cmdMove: UInt16     = 0x0101
    static let cmdStop: UInt16     = 0x0105
    static let cmdLedSet: UInt16   = 0x0201
    static let cmdLedOff: UInt16   = 0x0203
    static let cmdFaceExpr: UInt16 = 0x0204
    static let cmdSpeak: UInt16    = 0x0206
    static let cmdServoSet: UInt16 = 0x0107
    static let cmdHeartbeat: UInt16 = 0x0001

    // ── CRC-16/CCITT ───────────────────────────────────────────

    private static let crcTable: [UInt16] = {
        var table = [UInt16](repeating: 0, count: 256)
        for i in 0..<256 {
            var crc = UInt16(i) << 8
            for _ in 0..<8 {
                crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1
            }
            table[i] = crc
        }
        return table
    }()

    static func crc16(_ data: Data) -> UInt16 {
        var crc: UInt16 = 0xFFFF
        for byte in data {
            let idx = Int((UInt16(crc >> 8) ^ UInt16(byte)) & 0xFF)
            crc = (crc << 8) ^ crcTable[idx]
        }
        return crc
    }

    // ── Types ──────────────────────────────────────────────────

    struct BcpCommand {
        let cmdId: UInt16
        let payload: Data

        var payloadLen: UInt8 { UInt8(payload.count) }
        var wireLen: Int { 3 + payload.count }
    }

    struct BcpFrame {
        let seqNo: UInt16
        let commands: [BcpCommand]

        var cmdCount: Int { commands.count }
        var totalLen: Int {
            headerLen + commands.reduce(0) { $0 + $1.wireLen } + crcLen
        }
    }

    // ── Encode ─────────────────────────────────────────────────

    static func encode(frame: BcpFrame) -> Data {
        var data = Data(capacity: frame.totalLen)

        // Header
        data.append(magic)
        data.append(version)
        data.append(contentsOf: writeU16LE(UInt16(frame.totalLen)))
        data.append(contentsOf: writeU16LE(frame.seqNo))
        data.append(UInt8(frame.cmdCount))
        data.append(0) // reserved

        // Commands
        for cmd in frame.commands {
            data.append(contentsOf: writeU16LE(cmd.cmdId))
            data.append(cmd.payloadLen)
            data.append(cmd.payload)
        }

        // CRC over everything except CRC itself
        let crc = crc16(data)
        data.append(contentsOf: writeU16LE(crc))

        return data
    }

    // ── Helpers ────────────────────────────────────────────────

    private static func writeU16LE(_ value: UInt16) -> [UInt8] {
        [UInt8(value & 0xFF), UInt8((value >> 8) & 0xFF)]
    }
}
