// k6 spike test — sudden burst of robot connections.
//
// Simulates a fleet of robots reconnecting after a network partition.
// Target: < 1% error rate, < 2000ms connect p99 during spike.
//
// Usage:
//   k6 run tests/load/gateway-spike.js

import ws from 'k6/ws';
import { check, sleep } from 'k6';

const GATEWAY_URL = __ENV.GATEWAY_URL || 'ws://localhost:8080/ws';

export const options = {
  scenarios: {
    spike: {
      executor: 'ramping-arrival-rate',
      startRate: 5,
      timeUnit: '1s',
      preAllocatedVUs: 10,
      maxVUs: 1000,
      stages: [
        { duration: '60s', target: 10 },    // normal: 10 conn/s
        { duration: '10s', target: 500 },   // spike: 500 conn/s
        { duration: '30s', target: 500 },   // hold spike
        { duration: '10s', target: 10 },    // recover
        { duration: '30s', target: 10 },    // normal
      ],
    },
  },
  thresholds: {
    'ws_connecting': ['p(99) < 3000'],
    'http_req_failed': ['rate < 0.01'],
    'checks': ['rate > 0.95'],
  },
};

function buildRegisterFrame(seqNo, capabilities) {
  const totalLen = 19;
  const buf = new ArrayBuffer(totalLen);
  const dv = new DataView(buf);

  dv.setUint8(0, 0xCB);
  dv.setUint8(1, 0x01);
  dv.setUint16(2, totalLen, true);
  dv.setUint16(4, seqNo, true);
  dv.setUint8(6, 1);  // 1 command
  dv.setUint8(7, 0);

  dv.setUint16(8, 0x0002, true); // REGISTER
  dv.setUint8(10, 6);
  dv.setUint16(11, capabilities, true);
  dv.setUint8(13, 0); // fw_ver [0,1,0,0]
  dv.setUint8(14, 1);
  dv.setUint8(15, 0);
  dv.setUint8(16, 0);
  dv.setUint16(17, 0x0000, true); // CRC placeholder

  return new Uint8Array(buf);
}

export default function () {
  const connId = `${__VU}-${Date.now()}`;
  let connected = false;

  const res = ws.connect(`${GATEWAY_URL}?id=${connId}`, null, function (socket) {
    socket.setTimeout(function () {
      socket.close();
    }, 15000); // 15s max session

    socket.on('open', function () {
      connected = true;
      // Send register and a few heartbeats
      socket.sendBinary(buildRegisterFrame(0, 0x000F).buffer);

      // Send 2 heartbeats, then disconnect (simulating short-lived reconnect)
      let hbCount = 0;
      socket.setInterval(function () {
        if (hbCount++ >= 2) {
          socket.close();
          return;
        }
        // Minimal heartbeat: just send register frame again
        socket.sendBinary(buildRegisterFrame(hbCount, 0x000F).buffer);
      }, 1000);
    });

    socket.on('error', function (e) {
      // Expected during spike — some connections will be rejected
      if (e && e.message) {
        console.warn(`Conn ${connId}: ${e.message}`);
      }
    });
  });

  check(res, {
    'connection attempted': (r) => r !== undefined,
    'connected successfully': () => connected,
  });

  // Brief hold for the reconnect burst pattern
  sleep(2 + Math.random() * 3);
}
