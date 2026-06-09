/**
 * BCP (Bundle Command Protocol) C implementation.
 *
 * Matches the Rust bcp-core crate byte-for-byte.
 * Designed for ESP32-S3 / FreeRTOS — no dynamic allocation.
 *
 * Frame format (little-endian):
 *   [Magic(1)] [Version(1)] [TotalLen(2)] [SeqNo(2)] [CmdCount(1)] [Reserved(1)]
 *   [CmdID(2)][PayloadLen(1)][Payload...] ...
 *   [CRC16(2)]
 */

#ifndef BCP_CODEC_H
#define BCP_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ────────────────────────────────────────────── */

#define BCP_MAGIC                0xCB
#define BCP_VERSION              0x01
#define BCP_HEADER_LEN           8
#define BCP_CRC_LEN              2
#define BCP_MAX_FRAME_LEN        1024
#define BCP_MIN_FRAME_LEN        (BCP_HEADER_LEN + BCP_CRC_LEN)
#define BCP_MAX_CMDS_PER_FRAME   32
#define BCP_MAX_PAYLOAD_LEN      255
#define BCP_CMD_HEADER_LEN       3   /* CmdID(2) + PayloadLen(1) */

/* Command groups */
#define BCP_CMD_HEARTBEAT        0x0001
#define BCP_CMD_REGISTER         0x0002
#define BCP_CMD_REG_ACK          0x0003
#define BCP_CMD_PING             0x0004
#define BCP_CMD_PONG             0x0005
#define BCP_CMD_RESET            0x0006
#define BCP_CMD_OTA_START        0x0007
#define BCP_CMD_OTA_CHUNK        0x0008
#define BCP_CMD_OTA_DONE         0x0009
#define BCP_CMD_ERROR            0x00FF

#define BCP_CMD_MOVE             0x0101
#define BCP_CMD_MOVE_TO          0x0102
#define BCP_CMD_STOP             0x0103
#define BCP_CMD_SERVO_SET        0x0104
#define BCP_CMD_SERVO_BATCH      0x0105
#define BCP_CMD_HEAD_PAN_TILT    0x0106

#define BCP_CMD_LED_SET          0x0201
#define BCP_CMD_LED_PATTERN      0x0202
#define BCP_CMD_LED_OFF          0x0203
#define BCP_CMD_FACE_EXPR        0x0204
#define BCP_CMD_FACE_CUSTOM      0x0205
#define BCP_CMD_SPEAK            0x0206
#define BCP_CMD_TTS_TEXT         0x0207

#define BCP_CMD_ENV_DATA         0x0301
#define BCP_CMD_MOTION_EVENT     0x0302
#define BCP_CMD_AUDIO_EVENT      0x0303
#define BCP_CMD_AUDIO_STREAM     0x0304
#define BCP_CMD_IMAGE_SNAPSHOT   0x0305
#define BCP_CMD_DEPTH_DATA       0x0306
#define BCP_CMD_TOUCH_EVENT      0x0307
#define BCP_CMD_IMU_DATA         0x0308
#define BCP_CMD_OBSTACLE         0x0309

#define BCP_CMD_TASK_ASSIGN      0x0401
#define BCP_CMD_TASK_STATUS      0x0402
#define BCP_CMD_TASK_CANCEL      0x0403
#define BCP_CMD_SWARM_FORM       0x0404
#define BCP_CMD_PEER_MSG         0x0405

/* Diagnostic commands */
#define BCP_CMD_LOG_UPLOAD       0x0501
#define BCP_CMD_DIAG_REQ         0x0502
#define BCP_CMD_DIAG_RESP        0x0503
#define BCP_CMD_LOG_EVENT        0x0504

/* Reserved flag */
#define BCP_FLAG_NO_ACK          0x01

/* ── Types ────────────────────────────────────────────────── */

typedef enum {
    BCP_OK = 0,
    BCP_ERR_BAD_MAGIC,
    BCP_ERR_BAD_VERSION,
    BCP_ERR_INCOMPLETE,
    BCP_ERR_CRC_MISMATCH,
    BCP_ERR_BAD_CMD_ID,
    BCP_ERR_PAYLOAD_TOO_LARGE,
    BCP_ERR_TOO_MANY_COMMANDS,
    BCP_ERR_BUFFER_FULL,
    BCP_ERR_FRAME_TOO_LARGE,
    BCP_ERR_BAD_FRAME_LEN,
} bcp_error_t;

typedef enum {
    BCP_DIR_FORWARD = 0,
    BCP_DIR_BACKWARD,
    BCP_DIR_LEFT,
    BCP_DIR_RIGHT,
    BCP_DIR_ROTATE_LEFT,
    BCP_DIR_ROTATE_RIGHT,
    BCP_DIR_STOP,
    BCP_DIR_FORWARD_LEFT,
    BCP_DIR_FORWARD_RIGHT,
    BCP_DIR_BACKWARD_LEFT,
    BCP_DIR_BACKWARD_RIGHT,
} bcp_direction_t;

typedef enum {
    BCP_LED_SOLID = 0,
    BCP_LED_BREATHING,
    BCP_LED_FLOW,
    BCP_LED_BLINK,
    BCP_LED_RAINBOW,
    BCP_LED_KNIGHT_RIDER,
    BCP_LED_PARTY,
} bcp_led_mode_t;

typedef enum {
    BCP_FACE_NEUTRAL = 0,
    BCP_FACE_HAPPY,
    BCP_FACE_SAD,
    BCP_FACE_SURPRISED,
    BCP_FACE_ANGRY,
    BCP_FACE_CONFUSED,
    BCP_FACE_SLEEPY,
    BCP_FACE_EXPR_COUNT,
} bcp_face_expr_t;

/* ── Command payload structs ─────────────────────────────── */

typedef struct {
    uint8_t status;
    uint8_t battery;
    int8_t  rssi;
    uint16_t task_id;
} bcp_heartbeat_t;

typedef struct {
    uint16_t capabilities;
    uint8_t  firmware_version[4];
} bcp_register_t;

typedef struct {
    uint16_t short_id;
    uint16_t heartbeat_interval;
} bcp_reg_ack_t;

typedef struct {
    bcp_direction_t direction;
    uint8_t         speed;
} bcp_move_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t speed;
} bcp_move_to_t;

typedef struct {
    uint8_t emergency; /* bool */
} bcp_stop_t;

typedef struct {
    uint8_t  id;
    uint16_t angle;
} bcp_servo_set_t;

typedef struct {
    uint8_t pan;
    uint8_t tilt;
} bcp_head_pan_tilt_t;

typedef struct {
    uint16_t id;
    uint8_t  r, g, b;
} bcp_led_set_t;

typedef struct {
    bcp_led_mode_t mode;
    uint8_t        speed;
    uint8_t        r, g, b;
} bcp_led_pattern_t;

typedef struct {
    bcp_face_expr_t expr;
} bcp_face_expr_cmd_t;

typedef struct {
    int16_t  temp;     /* deci-Celsius */
    uint16_t humi;     /* deci-percent */
    uint32_t pressure; /* Pa */
    uint32_t light;
    uint16_t air;
} bcp_env_data_t;

typedef struct {
    uint8_t detect_type;
    uint8_t confidence;
} bcp_motion_event_t;

typedef struct {
    uint8_t  event_type;
    uint16_t energy;
} bcp_audio_event_t;

typedef struct {
    uint8_t zone;
    uint8_t pressure;
    uint8_t state;
} bcp_touch_event_t;

typedef struct {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} bcp_imu_data_t;

typedef struct {
    uint8_t  direction;
    uint16_t distance;
} bcp_obstacle_t;

typedef struct {
    uint16_t task_id;
    uint8_t  status;
    uint8_t  progress;
} bcp_task_status_t;

typedef struct {
    uint8_t  task_type;
    uint8_t  priority;
    uint8_t  params[BCP_MAX_PAYLOAD_LEN - 2];
    uint8_t  params_len;
} bcp_task_assign_t;

/* ── Generic command ─────────────────────────────────────── */

/** Unified command type with tagged union. */
typedef struct {
    uint16_t cmd_id;
    uint8_t  payload_len;
    union {
        bcp_heartbeat_t     heartbeat;
        bcp_register_t      reg;
        bcp_reg_ack_t       reg_ack;
        bcp_move_t          move;
        bcp_move_to_t       move_to;
        bcp_stop_t          stop;
        bcp_servo_set_t     servo_set;
        bcp_head_pan_tilt_t head;
        bcp_led_set_t       led_set;
        bcp_led_pattern_t   led_pattern;
        bcp_face_expr_cmd_t face;
        bcp_env_data_t      env;
        bcp_motion_event_t  motion;
        bcp_audio_event_t   audio;
        bcp_touch_event_t   touch;
        bcp_imu_data_t      imu;
        bcp_obstacle_t      obstacle;
        bcp_task_status_t   task_status;
        bcp_task_assign_t   task_assign;
        uint8_t             raw[BCP_MAX_PAYLOAD_LEN];
    } payload;
} bcp_command_t;

/* ── Frame ────────────────────────────────────────────────── */

typedef struct {
    uint8_t      version;
    uint16_t     seq_no;
    uint8_t      cmd_count;
    uint8_t      reserved;   /* bit0 = NO_ACK */
    bcp_command_t commands[BCP_MAX_CMDS_PER_FRAME];
} bcp_frame_t;

/* ── API ──────────────────────────────────────────────────── */

/** Compute CRC-16/CCITT over data[0..len-1]. */
uint16_t bcp_crc16(const uint8_t *data, size_t len);

/** Encode a frame into buf. Returns number of bytes written, or negative error. */
int bcp_encode(const bcp_frame_t *frame, uint8_t *buf, size_t buf_len);

/** Decode a frame from buf. Returns bytes consumed, or negative error.
 *  On success, *out_frame is populated. */
int bcp_decode(const uint8_t *buf, size_t buf_len, bcp_frame_t *out_frame);

/** Initialize an empty frame. */
void bcp_frame_init(bcp_frame_t *frame, uint16_t seq_no);

/** Add a command to a frame. Returns 0 on success, negative on error. */
int bcp_frame_push(bcp_frame_t *frame, const bcp_command_t *cmd);

/** Compute total wire size of a frame. */
size_t bcp_frame_wire_len(const bcp_frame_t *frame);

/** Encode a single heartbeat command payload. */
void bcp_encode_heartbeat(bcp_command_t *cmd, uint8_t status, uint8_t battery,
                           int8_t rssi, uint16_t task_id);

/* ── Delta heartbeat ──────────────────────────────────────── */

/** Delta heartbeat variant type. */
typedef enum {
    HEARTBEAT_FULL = 0,         /* 6B: [0x00, status, battery, rssi, task_id(2B)] */
    HEARTBEAT_NO_CHANGE = 1,    /* 1B: [0x01] — nothing changed */
    HEARTBEAT_BATTERY_ONLY = 2, /* 2B: [0x02, battery] — only battery changed */
} heartbeat_delta_type_t;

/** Delta-encoded heartbeat. */
typedef struct {
    heartbeat_delta_type_t type;
    uint8_t  status;
    uint8_t  battery;
    int8_t   rssi;
    uint16_t task_id;
} heartbeat_delta_t;

/** Tracks last heartbeat state and computes delta encodings.
 *  Saves ~60% bandwidth vs full heartbeat every time. */
typedef struct {
    bool     has_last;
    uint8_t  last_status;
    uint8_t  last_battery;
    int8_t   last_rssi;
    uint16_t last_task_id;
} heartbeat_differ_t;

/** Initialize a heartbeat differ. */
void heartbeat_differ_init(heartbeat_differ_t *differ);

/** Compute delta encoding of current state vs last transmitted.
 *  First call always returns HEARTBEAT_FULL.
 *  Updates internal last-state after comparison. */
void heartbeat_differ_diff(heartbeat_differ_t *differ, heartbeat_delta_t *out,
                            uint8_t status, uint8_t battery,
                            int8_t rssi, uint16_t task_id);

/** Encode a delta heartbeat into a BCP command. */
void bcp_encode_heartbeat_delta(bcp_command_t *cmd, const heartbeat_delta_t *delta);

/** Encode an IMU data command payload. */
void bcp_encode_imu(bcp_command_t *cmd, int16_t ax, int16_t ay, int16_t az,
                     int16_t gx, int16_t gy, int16_t gz);

/** Encode an obstacle detection command. */
void bcp_encode_obstacle(bcp_command_t *cmd, uint8_t direction, uint16_t distance);

/** Encode an environment sensor command. */
void bcp_encode_env(bcp_command_t *cmd, int16_t temp, uint16_t humi,
                     uint32_t pressure, uint32_t light, uint16_t air);

/** Get human-readable error string. */
const char *bcp_error_str(bcp_error_t err);

#ifdef __cplusplus
}
#endif

#endif /* BCP_CODEC_H */
