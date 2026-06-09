package com.companion.robot.ble

/**
 * BCP (Bundle Command Protocol) codec — Kotlin implementation.
 *
 * Wire format (little-endian):
 *   Header:  Magic(1B) + Version(1B) + TotalLen(2B LE) + SeqNo(2B LE) + CmdCount(1B) + Reserved(1B)
 *   Commands: CmdID(2B LE) + PayloadLen(1B) + Payload(N bytes)
 *   CRC: CRC-16/CCITT over bytes 0..TotalLen-2
 */

object BcpCodec {
    // Protocol constants
    const val MAGIC: Byte = 0xCB.toByte()
    const val VERSION: Byte = 0x01
    const val HEADER_LEN = 8
    const val CRC_LEN = 2
    const val MAX_FRAME_LEN = 1024
    const val MAX_COMMANDS = 32
    const val MAX_PAYLOAD_LEN = 255

    // Command IDs
    const val CMD_HEARTBEAT: Int = 0x0001

    // ── CRC-16/CCITT lookup table ──────────────────────────────

    private val crcTable: Array<Short> by lazy {
        Array(256) { i ->
            var crc = (i shl 8).toShort()
            repeat(8) {
                crc = if ((crc.toInt() and 0x8000) != 0) {
                    ((crc.toInt() shl 1) xor 0x1021).toShort()
                } else {
                    (crc.toInt() shl 1).toShort()
                }
            }
            crc
        }
    }

    fun crc16(data: ByteArray, offset: Int = 0, len: Int = data.size): Short {
        var crc: Short = 0xFFFF.toShort()
        for (i in offset until offset + len) {
            val idx = ((crc.toInt() shr 8) xor (data[i].toInt() and 0xFF)) and 0xFF
            crc = ((crc.toInt() shl 8) xor crcTable[idx].toInt()).toShort()
        }
        return crc
    }

    // ── Encode ─────────────────────────────────────────────────

    data class BcpCommand(val cmdId: Int, val payload: ByteArray) {
        val payloadLen: Int get() = payload.size
        val wireLen: Int get() = 3 + payload.size // cmdId(2) + payloadLen(1) + payload

        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is BcpCommand) return false
            return cmdId == other.cmdId && payload.contentEquals(other.payload)
        }

        override fun hashCode(): Int = cmdId * 31 + payload.contentHashCode()
    }

    data class BcpFrame(
        val seqNo: Int,
        val commands: List<BcpCommand>
    ) {
        val cmdCount: Int get() = commands.size
        val totalLen: Int get() = HEADER_LEN + commands.sumOf { it.wireLen } + CRC_LEN

        companion object {
            fun create(seqNo: Int, vararg commands: BcpCommand): BcpFrame {
                return BcpFrame(seqNo, commands.toList())
            }
        }
    }

    fun encode(frame: BcpFrame, buf: ByteArray): Int {
        var pos = 0

        // Header
        buf[pos++] = MAGIC
        buf[pos++] = VERSION
        writeU16LE(buf, pos, frame.totalLen); pos += 2
        writeU16LE(buf, pos, frame.seqNo); pos += 2
        buf[pos++] = frame.cmdCount.toByte()
        buf[pos++] = 0 // reserved

        // Commands
        for (cmd in frame.commands) {
            writeU16LE(buf, pos, cmd.cmdId); pos += 2
            buf[pos++] = cmd.payloadLen.toByte()
            cmd.payload.copyInto(buf, pos)
            pos += cmd.payload.size
        }

        // CRC over everything except CRC field itself
        val crc = crc16(buf, 0, pos)
        writeU16LE(buf, pos, crc.toInt()); pos += 2

        return pos
    }

    // ── Helpers ────────────────────────────────────────────────

    private fun writeU16LE(buf: ByteArray, offset: Int, value: Int) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
    }

    fun readU16LE(buf: ByteArray, offset: Int): Int =
        (buf[offset].toInt() and 0xFF) or ((buf[offset + 1].toInt() and 0xFF) shl 8)
}
