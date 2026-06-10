// k6 WebSocket scaling test for companion robot gateway.
//
// Simulates N robots connecting, registering, and sending periodic heartbeats.
// Staged ramp: 50 → 200 → 500 concurrent connections.
//
// Usage:
//   k6 run tests/load/gateway-scale.js

import ws from 'k6/ws';
import { check, sleep, fail } from 'k6';

const GATEWAY_URL = __ENV.GATEWAY_URL || 'ws://localhost:8080/ws';
const MAX_VUS = parseInt(__ENV.MAX_VUS || '500');
const HEARTBEAT_INTERVAL_S = 5;

export const options = {
  stages: [
    { duration: '30s', target: 50 },   // ramp to 50
    { duration: '60s', target: 200 },  // ramp to 200
    { duration: '60s', target: 500 },  // ramp to 500
    { duration: '120s', target: 500 }, // hold 500
    { duration: '30s', target: 0 },    // ramp down
  ],
  thresholds: {
    'ws_connecting': ['p(95) < 2000'],       // connect p95 < 2s
    'ws_session_duration': ['p(95) > 5000'], // sessions survive > 5s
    'checks': ['rate > 0.95'],               // 95% check pass rate
  },
  noConnectionReuse: true,
};

// Simple BCP heartbeat frame builder (minimal JS impl for testing)
// Format: [MAGIC 0xCB] [VERSION 1] [TOTAL_LEN LE] [SEQ LE] [CMD_COUNT] [RSVD]
//         [CMD_ID LE] [PAYLOAD_LEN] [PAYLOAD...] [CRC LE]
function buildHeartbeatFrame(seqNo, status, battery, rssi, taskId) {
  // Heartbeat CmdID = 0x0001, payload = 5 bytes
  const totalLen = 8 + 3 + 5 + 2; // header + cmd_hdr + payload + crc = 18
  const buf = new ArrayBuffer(totalLen);
  const dv = new DataView(buf);

  // Header
  dv.setUint8(0, 0xCB);          // magic
  dv.setUint8(1, 0x01);          // version
  dv.setUint16(2, totalLen, true); // total_len LE
  dv.setUint16(4, seqNo, true);    // seq_no LE
  dv.setUint8(6, 1);             // cmd_count = 1
  dv.setUint8(7, 0);             // reserved

  // Command header
  dv.setUint16(8, 0x0001, true); // CmdID = HEARTBEAT
  dv.setUint8(10, 5);           // payload_len = 5

  // Payload
  dv.setUint8(11, status);      // status
  dv.setUint8(12, battery);     // battery
  dv.setInt8(13, rssi);         // rssi
  dv.setUint16(14, taskId, true); // task_id

  // CRC-16/CCITT placeholder (simplified; real test needs full CRC)
  dv.setUint16(16, 0x0000, true);

  return new Uint8Array(buf);
}

// Build a simple registration frame
function buildRegisterFrame(seqNo, capabilities, fwVer) {
  const totalLen = 8 + 3 + 6 + 2; // header + cmd_hdr + 2(caps)+4(fw) + crc = 19
  const buf = new ArrayBuffer(totalLen);
  const dv = new DataView(buf);

  dv.setUint8(0, 0xCB);
  dv.setUint8(1, 0x01);
  dv.setUint16(2, totalLen, true);
  dv.setUint16(4, seqNo, true);
  dv.setUint8(6, 1);
  dv.setUint8(7, 0);

  dv.setUint16(8, 0x0002, true); // CmdID = REGISTER
  dv.setUint8(10, 6);           // payload_len
  dv.setUint16(11, capabilities, true);
  dv.setUint8(13, fwVer[0]);
  dv.setUint8(14, fwVer[1]);
  dv.setUint8(15, fwVer[2]);
  dv.setUint8(16, fwVer[3]);

  dv.setUint16(17, 0x0000, true); // CRC placeholder
  return new Uint8Array(buf);
}

export default function () {
  const vuId = __VU;  // unique per VU — use as robot short_id

  const url = `${GATEWAY_URL}?id=${vuId}`;
  let seqNo = 0;
  let registered = false;

  const res = ws.connect(url, null, function (socket) {
    socket.setTimeout(function () {
      fail(`VU ${vuId}: WebSocket timed out`);
    }, 30000);

    socket.on('open', function () {
      // Send registration
      const regFrame = buildRegisterFrame(seqNo++, 0x000F, [0, 1, 0, 0]);
      socket.sendBinary(regFrame.buffer);

      // Start heartbeat loop
      socket.setInterval(function () {
        const hb = buildHeartbeatFrame(seqNo++, 1, 80 + (vuId % 20), -40, 0);
        socket.sendBinary(hb.buffer);
      }, HEARTBEAT_INTERVAL_S * 1000);
    });

    socket.on('message', function (data) {
      // Check for RegAck (CmdID=0x0003) — simplified check
      if (data instanceof ArrayBuffer && data.byteLength >= 10) {
        const dv = new DataView(data);
        if (dv.getUint8(0) === 0xCB) {
          // Valid BCP frame received
          registered = true;
        }
      }
    });

    socket.on('close', function () {
      // Disconnect handled by k6 lifecycle
    });

    socket.on('error', function (e) {
      console.error(`VU ${vuId}: WebSocket error: ${e}`);
    });
  });

  check(res, {
    'WebSocket connected': (r) => r && r.status === 101,
  });

  // Simulate robot session with heartbeats
  sleep(30 + Math.random() * 30); // hold for 30-60s
}
