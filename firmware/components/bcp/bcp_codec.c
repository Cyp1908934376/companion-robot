/**
 * BCP codec implementation — matches bcp-core (Rust) byte-for-byte.
 *
 * CRC-16/CCITT with polynomial 0x1021, initial 0xFFFF.
 * All multi-byte fields little-endian.
 */

#include "bcp_codec.h"
#include <string.h>

/* ── CRC-16/CCITT lookup table ────────────────────────────── */

static const uint16_t crc_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

uint16_t bcp_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc >> 8) ^ data[i]);
        crc = (uint16_t)((crc << 8) ^ crc_table[idx]);
    }
    return crc;
}

/* ── Helper: write little-endian multi-byte values ────────── */

static inline void write_u16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

static inline void write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static inline void write_i16(uint8_t *buf, int16_t val) {
    write_u16(buf, (uint16_t)val);
}

static inline uint16_t read_u16(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline uint32_t read_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline int16_t read_i16(const uint8_t *buf) {
    return (int16_t)read_u16(buf);
}

/* ── Command payload size ─────────────────────────────────── */

static size_t cmd_payload_len(uint16_t cmd_id, const bcp_command_t *cmd) {
    switch (cmd_id) {
    case BCP_CMD_HEARTBEAT:     return 5;
    case BCP_CMD_REGISTER:      return 6;
    case BCP_CMD_REG_ACK:       return 4;
    case BCP_CMD_PING:          return 4;
    case BCP_CMD_PONG:          return 4;
    case BCP_CMD_RESET:         return 1;
    case BCP_CMD_OTA_START:     return 20;
    case BCP_CMD_OTA_CHUNK:     return 4 + cmd->payload_len - 4; /* offset(4) + data */
    case BCP_CMD_OTA_DONE:      return 0;
    case BCP_CMD_ERROR:         return 5;
    case BCP_CMD_MOVE:          return 2;
    case BCP_CMD_MOVE_TO:       return 5;
    case BCP_CMD_STOP:          return 1;
    case BCP_CMD_SERVO_SET:     return 3;
    case BCP_CMD_SERVO_BATCH:   return cmd->payload_len;
    case BCP_CMD_HEAD_PAN_TILT: return 2;
    case BCP_CMD_LED_SET:       return 5;
    case BCP_CMD_LED_PATTERN:   return 5;
    case BCP_CMD_LED_OFF:       return 0;
    case BCP_CMD_FACE_EXPR:     return 1;
    case BCP_CMD_FACE_CUSTOM:   return cmd->payload_len;
    case BCP_CMD_SPEAK:         return 2 + cmd->payload_len - 2;
    case BCP_CMD_TTS_TEXT:      return 1 + cmd->payload_len - 1;
    case BCP_CMD_ENV_DATA:      return 14;
    case BCP_CMD_MOTION_EVENT:  return 2;
    case BCP_CMD_AUDIO_EVENT:   return 3;
    case BCP_CMD_AUDIO_STREAM:  return 1 + cmd->payload_len - 1;
    case BCP_CMD_IMAGE_SNAPSHOT:return 5 + cmd->payload_len - 5;
    case BCP_CMD_DEPTH_DATA:    return 4 + cmd->payload_len - 4;
    case BCP_CMD_TOUCH_EVENT:   return 3;
    case BCP_CMD_IMU_DATA:      return 12;
    case BCP_CMD_OBSTACLE:      return 3;
    case BCP_CMD_TASK_ASSIGN:   return 2 + cmd->payload_len - 2;
    case BCP_CMD_TASK_STATUS:   return 4;
    case BCP_CMD_TASK_CANCEL:   return 2;
    case BCP_CMD_SWARM_FORM:    return 1 + cmd->payload_len - 1;
    case BCP_CMD_PEER_MSG:      return 2 + cmd->payload_len - 2;
    default:                    return 0;
    }
}

/* ── Encode command payload into buffer ───────────────────── */

static void encode_cmd_payload(uint16_t cmd_id, const bcp_command_t *cmd, uint8_t *buf) {
    switch (cmd_id) {
    case BCP_CMD_HEARTBEAT:
        buf[0] = cmd->payload.heartbeat.status;
        buf[1] = cmd->payload.heartbeat.battery;
        buf[2] = (uint8_t)cmd->payload.heartbeat.rssi;
        write_u16(&buf[3], cmd->payload.heartbeat.task_id);
        break;
    case BCP_CMD_REGISTER:
        write_u16(&buf[0], cmd->payload.reg.capabilities);
        memcpy(&buf[2], cmd->payload.reg.firmware_version, 4);
        break;
    case BCP_CMD_REG_ACK:
        write_u16(&buf[0], cmd->payload.reg_ack.short_id);
        write_u16(&buf[2], cmd->payload.reg_ack.heartbeat_interval);
        break;
    case BCP_CMD_PING:
    case BCP_CMD_PONG:
        write_u32(&buf[0], 0); /* timestamp */
        break;
    case BCP_CMD_RESET:
        buf[0] = 0;
        break;
    case BCP_CMD_MOVE:
        buf[0] = (uint8_t)cmd->payload.move.direction;
        buf[1] = cmd->payload.move.speed;
        break;
    case BCP_CMD_MOVE_TO:
        write_i16(&buf[0], cmd->payload.move_to.x);
        write_i16(&buf[2], cmd->payload.move_to.y);
        buf[4] = cmd->payload.move_to.speed;
        break;
    case BCP_CMD_STOP:
        buf[0] = cmd->payload.stop.emergency;
        break;
    case BCP_CMD_SERVO_SET:
        buf[0] = cmd->payload.servo_set.id;
        write_u16(&buf[1], cmd->payload.servo_set.angle);
        break;
    case BCP_CMD_HEAD_PAN_TILT:
        buf[0] = cmd->payload.head.pan;
        buf[1] = cmd->payload.head.tilt;
        break;
    case BCP_CMD_LED_SET:
        write_u16(&buf[0], cmd->payload.led_set.id);
        buf[2] = cmd->payload.led_set.r;
        buf[3] = cmd->payload.led_set.g;
        buf[4] = cmd->payload.led_set.b;
        break;
    case BCP_CMD_LED_PATTERN:
        buf[0] = (uint8_t)cmd->payload.led_pattern.mode;
        buf[1] = cmd->payload.led_pattern.speed;
        buf[2] = cmd->payload.led_pattern.r;
        buf[3] = cmd->payload.led_pattern.g;
        buf[4] = cmd->payload.led_pattern.b;
        break;
    case BCP_CMD_LED_OFF:
        break;
    case BCP_CMD_FACE_EXPR:
        buf[0] = (uint8_t)cmd->payload.face.expr;
        break;
    case BCP_CMD_ENV_DATA:
        write_i16(&buf[0], cmd->payload.env.temp);
        write_u16(&buf[2], cmd->payload.env.humi);
        write_u32(&buf[4], cmd->payload.env.pressure);
        write_u32(&buf[8], cmd->payload.env.light);
        write_u16(&buf[12], cmd->payload.env.air);
        break;
    case BCP_CMD_MOTION_EVENT:
        buf[0] = cmd->payload.motion.detect_type;
        buf[1] = cmd->payload.motion.confidence;
        break;
    case BCP_CMD_AUDIO_EVENT:
        buf[0] = cmd->payload.audio.event_type;
        write_u16(&buf[1], cmd->payload.audio.energy);
        break;
    case BCP_CMD_TOUCH_EVENT:
        buf[0] = cmd->payload.touch.zone;
        buf[1] = cmd->payload.touch.pressure;
        buf[2] = cmd->payload.touch.state;
        break;
    case BCP_CMD_IMU_DATA:
        write_i16(&buf[0], cmd->payload.imu.ax);
        write_i16(&buf[2], cmd->payload.imu.ay);
        write_i16(&buf[4], cmd->payload.imu.az);
        write_i16(&buf[6], cmd->payload.imu.gx);
        write_i16(&buf[8], cmd->payload.imu.gy);
        write_i16(&buf[10], cmd->payload.imu.gz);
        break;
    case BCP_CMD_OBSTACLE:
        buf[0] = cmd->payload.obstacle.direction;
        write_u16(&buf[1], cmd->payload.obstacle.distance);
        break;
    case BCP_CMD_TASK_STATUS:
        write_u16(&buf[0], cmd->payload.task_status.task_id);
        buf[2] = cmd->payload.task_status.status;
        buf[3] = cmd->payload.task_status.progress;
        break;
    case BCP_CMD_TASK_CANCEL:
        write_u16(&buf[0], 0);
        break;
    default:
        /* For variable-length payloads, copy raw bytes */
        memcpy(buf, cmd->payload.raw, cmd->payload_len);
        break;
    }
}

/* ── Decode command payload from buffer ───────────────────── */

static void decode_cmd_payload(uint16_t cmd_id, bcp_command_t *cmd,
                                const uint8_t *payload, size_t len) {
    memset(&cmd->payload, 0, sizeof(cmd->payload));
    cmd->payload_len = (uint8_t)len;

    switch (cmd_id) {
    case BCP_CMD_HEARTBEAT:
        cmd->payload.heartbeat.status  = payload[0];
        cmd->payload.heartbeat.battery = payload[1];
        cmd->payload.heartbeat.rssi    = (int8_t)payload[2];
        cmd->payload.heartbeat.task_id = read_u16(&payload[3]);
        break;
    case BCP_CMD_REGISTER:
        cmd->payload.reg.capabilities = read_u16(&payload[0]);
        memcpy(cmd->payload.reg.firmware_version, &payload[2], 4);
        break;
    case BCP_CMD_REG_ACK:
        cmd->payload.reg_ack.short_id = read_u16(&payload[0]);
        cmd->payload.reg_ack.heartbeat_interval = read_u16(&payload[2]);
        break;
    case BCP_CMD_MOVE:
        cmd->payload.move.direction = (bcp_direction_t)payload[0];
        cmd->payload.move.speed     = payload[1];
        break;
    case BCP_CMD_MOVE_TO:
        cmd->payload.move_to.x     = read_i16(&payload[0]);
        cmd->payload.move_to.y     = read_i16(&payload[2]);
        cmd->payload.move_to.speed = payload[4];
        break;
    case BCP_CMD_STOP:
        cmd->payload.stop.emergency = payload[0];
        break;
    case BCP_CMD_SERVO_SET:
        cmd->payload.servo_set.id    = payload[0];
        cmd->payload.servo_set.angle = read_u16(&payload[1]);
        break;
    case BCP_CMD_HEAD_PAN_TILT:
        cmd->payload.head.pan  = payload[0];
        cmd->payload.head.tilt = payload[1];
        break;
    case BCP_CMD_LED_SET:
        cmd->payload.led_set.id = read_u16(&payload[0]);
        cmd->payload.led_set.r  = payload[2];
        cmd->payload.led_set.g  = payload[3];
        cmd->payload.led_set.b  = payload[4];
        break;
    case BCP_CMD_LED_PATTERN:
        cmd->payload.led_pattern.mode  = (bcp_led_mode_t)payload[0];
        cmd->payload.led_pattern.speed = payload[1];
        cmd->payload.led_pattern.r     = payload[2];
        cmd->payload.led_pattern.g     = payload[3];
        cmd->payload.led_pattern.b     = payload[4];
        break;
    case BCP_CMD_FACE_EXPR:
        cmd->payload.face.expr = (bcp_face_expr_t)payload[0];
        break;
    case BCP_CMD_ENV_DATA:
        cmd->payload.env.temp     = read_i16(&payload[0]);
        cmd->payload.env.humi     = read_u16(&payload[2]);
        cmd->payload.env.pressure = read_u32(&payload[4]);
        cmd->payload.env.light    = read_u32(&payload[8]);
        cmd->payload.env.air      = read_u16(&payload[12]);
        break;
    case BCP_CMD_MOTION_EVENT:
        cmd->payload.motion.detect_type = payload[0];
        cmd->payload.motion.confidence  = payload[1];
        break;
    case BCP_CMD_AUDIO_EVENT:
        cmd->payload.audio.event_type = payload[0];
        cmd->payload.audio.energy     = read_u16(&payload[1]);
        break;
    case BCP_CMD_TOUCH_EVENT:
        cmd->payload.touch.zone     = payload[0];
        cmd->payload.touch.pressure = payload[1];
        cmd->payload.touch.state    = payload[2];
        break;
    case BCP_CMD_IMU_DATA:
        cmd->payload.imu.ax = read_i16(&payload[0]);
        cmd->payload.imu.ay = read_i16(&payload[2]);
        cmd->payload.imu.az = read_i16(&payload[4]);
        cmd->payload.imu.gx = read_i16(&payload[6]);
        cmd->payload.imu.gy = read_i16(&payload[8]);
        cmd->payload.imu.gz = read_i16(&payload[10]);
        break;
    case BCP_CMD_OBSTACLE:
        cmd->payload.obstacle.direction = payload[0];
        cmd->payload.obstacle.distance  = read_u16(&payload[1]);
        break;
    case BCP_CMD_TASK_STATUS:
        cmd->payload.task_status.task_id  = read_u16(&payload[0]);
        cmd->payload.task_status.status   = payload[2];
        cmd->payload.task_status.progress = payload[3];
        break;
    case BCP_CMD_TASK_CANCEL:
        break;
    default:
        memcpy(cmd->payload.raw, payload, len);
        break;
    }
}

/* ── Public API ───────────────────────────────────────────── */

void bcp_frame_init(bcp_frame_t *frame, uint16_t seq_no) {
    memset(frame, 0, sizeof(*frame));
    frame->version = BCP_VERSION;
    frame->seq_no = seq_no;
}

int bcp_frame_push(bcp_frame_t *frame, const bcp_command_t *cmd) {
    if (frame->cmd_count >= BCP_MAX_CMDS_PER_FRAME) {
        return BCP_ERR_TOO_MANY_COMMANDS;
    }
    frame->commands[frame->cmd_count] = *cmd;
    frame->cmd_count++;
    return 0;
}

size_t bcp_frame_wire_len(const bcp_frame_t *frame) {
    size_t total = BCP_HEADER_LEN + BCP_CRC_LEN;
    for (int i = 0; i < frame->cmd_count; i++) {
        total += BCP_CMD_HEADER_LEN + cmd_payload_len(
            frame->commands[i].cmd_id, &frame->commands[i]);
    }
    return total;
}

int bcp_encode(const bcp_frame_t *frame, uint8_t *buf, size_t buf_len) {
    size_t total_len = bcp_frame_wire_len(frame);
    if (total_len > BCP_MAX_FRAME_LEN) return BCP_ERR_FRAME_TOO_LARGE;
    if (buf_len < total_len)           return BCP_ERR_BUFFER_FULL;

    /* Header */
    buf[0] = BCP_MAGIC;
    buf[1] = frame->version;
    write_u16(&buf[2], (uint16_t)total_len);
    write_u16(&buf[4], frame->seq_no);
    buf[6] = frame->cmd_count;
    buf[7] = frame->reserved;

    /* Commands */
    size_t pos = BCP_HEADER_LEN;
    for (int i = 0; i < frame->cmd_count; i++) {
        const bcp_command_t *cmd = &frame->commands[i];
        uint16_t cmd_id = cmd->cmd_id;
        size_t plen = cmd_payload_len(cmd_id, cmd);

        write_u16(&buf[pos], cmd_id);
        buf[pos + 2] = (uint8_t)plen;
        pos += BCP_CMD_HEADER_LEN;

        encode_cmd_payload(cmd_id, cmd, &buf[pos]);
        pos += plen;
    }

    /* CRC-16/CCITT */
    uint16_t crc = bcp_crc16(buf, total_len - BCP_CRC_LEN);
    write_u16(&buf[total_len - 2], crc);

    return (int)total_len;
}

int bcp_decode(const uint8_t *buf, size_t buf_len, bcp_frame_t *out_frame) {
    if (buf_len < BCP_MIN_FRAME_LEN) return BCP_ERR_INCOMPLETE;
    if (buf[0] != BCP_MAGIC)         return BCP_ERR_BAD_MAGIC;
    if (buf[1] != BCP_VERSION)       return BCP_ERR_BAD_VERSION;

    uint16_t total_len = read_u16(&buf[2]);
    if (total_len < BCP_MIN_FRAME_LEN || total_len > BCP_MAX_FRAME_LEN) {
        return BCP_ERR_BAD_FRAME_LEN;
    }
    if (buf_len < total_len) return BCP_ERR_INCOMPLETE;

    /* CRC check */
    uint16_t expected = read_u16(&buf[total_len - 2]);
    uint16_t computed = bcp_crc16(buf, total_len - BCP_CRC_LEN);
    if (expected != computed) return BCP_ERR_CRC_MISMATCH;

    /* Parse */
    uint16_t seq_no   = read_u16(&buf[4]);
    uint8_t  cmd_count = buf[6];
    uint8_t  reserved  = buf[7];

    if (cmd_count > BCP_MAX_CMDS_PER_FRAME) return BCP_ERR_TOO_MANY_COMMANDS;

    bcp_frame_init(out_frame, seq_no);
    out_frame->reserved = reserved;

    size_t pos = BCP_HEADER_LEN;
    size_t cmd_end = total_len - BCP_CRC_LEN;

    for (int i = 0; i < cmd_count; i++) {
        if (pos + BCP_CMD_HEADER_LEN > cmd_end) return BCP_ERR_INCOMPLETE;

        uint16_t cmd_id     = read_u16(&buf[pos]);
        uint8_t  payload_len = buf[pos + 2];
        pos += BCP_CMD_HEADER_LEN;

        if (pos + payload_len > cmd_end) return BCP_ERR_INCOMPLETE;

        bcp_command_t cmd;
        cmd.cmd_id = cmd_id;
        decode_cmd_payload(cmd_id, &cmd, &buf[pos], payload_len);
        pos += payload_len;

        int ret = bcp_frame_push(out_frame, &cmd);
        if (ret != 0) return ret;
    }

    return (int)total_len;
}

/* ── Convenience encoders ─────────────────────────────────── */

void bcp_encode_heartbeat(bcp_command_t *cmd, uint8_t status, uint8_t battery,
                           int8_t rssi, uint16_t task_id) {
    cmd->cmd_id = BCP_CMD_HEARTBEAT;
    cmd->payload_len = 5;
    cmd->payload.heartbeat.status  = status;
    cmd->payload.heartbeat.battery = battery;
    cmd->payload.heartbeat.rssi    = rssi;
    cmd->payload.heartbeat.task_id = task_id;
}

/* ── Delta heartbeat ──────────────────────────────────────── */

void heartbeat_differ_init(heartbeat_differ_t *differ) {
    differ->has_last = false;
    differ->last_status  = 0;
    differ->last_battery = 0;
    differ->last_rssi    = 0;
    differ->last_task_id = 0;
}

void heartbeat_differ_diff(heartbeat_differ_t *differ, heartbeat_delta_t *out,
                            uint8_t status, uint8_t battery,
                            int8_t rssi, uint16_t task_id) {
    if (!differ->has_last) {
        differ->has_last = true;
        out->type    = HEARTBEAT_FULL;
        out->status  = status;
        out->battery = battery;
        out->rssi    = rssi;
        out->task_id = task_id;
        goto save;
    }

    if (differ->last_status == status
        && differ->last_rssi == rssi
        && differ->last_task_id == task_id) {
        if (differ->last_battery == battery) {
            out->type = HEARTBEAT_NO_CHANGE;
        } else {
            out->type    = HEARTBEAT_BATTERY_ONLY;
            out->battery = battery;
        }
    } else {
        out->type    = HEARTBEAT_FULL;
        out->status  = status;
        out->battery = battery;
        out->rssi    = rssi;
        out->task_id = task_id;
    }

save:
    differ->last_status  = status;
    differ->last_battery = battery;
    differ->last_rssi    = rssi;
    differ->last_task_id = task_id;
}

void bcp_encode_heartbeat_delta(bcp_command_t *cmd, const heartbeat_delta_t *delta) {
    cmd->cmd_id = BCP_CMD_HEARTBEAT;

    switch (delta->type) {
    case HEARTBEAT_NO_CHANGE:
        cmd->payload_len = 1;
        cmd->payload.raw[0] = 0x01;
        break;
    case HEARTBEAT_BATTERY_ONLY:
        cmd->payload_len = 2;
        cmd->payload.raw[0] = 0x02;
        cmd->payload.raw[1] = delta->battery;
        break;
    case HEARTBEAT_FULL:
    default:
        cmd->payload_len = 6;
        cmd->payload.raw[0] = 0x00;
        cmd->payload.raw[1] = delta->status;
        cmd->payload.raw[2] = delta->battery;
        cmd->payload.raw[3] = (uint8_t)delta->rssi;
        cmd->payload.raw[4] = (uint8_t)(delta->task_id & 0xFF);
        cmd->payload.raw[5] = (uint8_t)((delta->task_id >> 8) & 0xFF);
        break;
    }
}

void bcp_encode_imu(bcp_command_t *cmd, int16_t ax, int16_t ay, int16_t az,
                     int16_t gx, int16_t gy, int16_t gz) {
    cmd->cmd_id = BCP_CMD_IMU_DATA;
    cmd->payload_len = 12;
    cmd->payload.imu.ax = ax;
    cmd->payload.imu.ay = ay;
    cmd->payload.imu.az = az;
    cmd->payload.imu.gx = gx;
    cmd->payload.imu.gy = gy;
    cmd->payload.imu.gz = gz;
}

void bcp_encode_obstacle(bcp_command_t *cmd, uint8_t direction, uint16_t distance) {
    cmd->cmd_id = BCP_CMD_OBSTACLE;
    cmd->payload_len = 3;
    cmd->payload.obstacle.direction = direction;
    cmd->payload.obstacle.distance  = distance;
}

void bcp_encode_env(bcp_command_t *cmd, int16_t temp, uint16_t humi,
                     uint32_t pressure, uint32_t light, uint16_t air) {
    cmd->cmd_id = BCP_CMD_ENV_DATA;
    cmd->payload_len = 14;
    cmd->payload.env.temp     = temp;
    cmd->payload.env.humi     = humi;
    cmd->payload.env.pressure = pressure;
    cmd->payload.env.light    = light;
    cmd->payload.env.air      = air;
}

const char *bcp_error_str(bcp_error_t err) {
    switch (err) {
    case BCP_OK:                return "OK";
    case BCP_ERR_BAD_MAGIC:     return "bad magic byte";
    case BCP_ERR_BAD_VERSION:   return "unsupported version";
    case BCP_ERR_INCOMPLETE:    return "incomplete frame";
    case BCP_ERR_CRC_MISMATCH:  return "CRC mismatch";
    case BCP_ERR_BAD_CMD_ID:    return "unknown command ID";
    case BCP_ERR_PAYLOAD_TOO_LARGE: return "payload too large";
    case BCP_ERR_TOO_MANY_COMMANDS: return "too many commands";
    case BCP_ERR_BUFFER_FULL:   return "buffer full";
    case BCP_ERR_FRAME_TOO_LARGE: return "frame too large";
    case BCP_ERR_BAD_FRAME_LEN: return "bad frame length";
    default:                    return "unknown error";
    }
}
