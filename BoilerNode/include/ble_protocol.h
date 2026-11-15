// Shared BLE contract between the display (central) and the boiler node (peripheral).
// Keep this file identical in both projects.
//
// Security model (application level, independent of BLE pairing):
//   1. Mutual challenge-response with a 256-bit pre-shared key (include/ble_key.h, same on both).
//   2. Per-connection session key derived from both nonces and the protocol version.
//   3. Every control/command/status frame is encrypted and authenticated with AES-256-CCM
//      under the session key, with a strictly increasing counter: forged, modified, replayed
//      or eavesdropped frames are useless.
#pragma once
#include <stdint.h>

#define BLE_PROTO_VERSION   2      // bump on any wire change; mismatched firmware fails the handshake cleanly

#define BLE_DEVICE_NAME     "CV-Boiler"
#define BLE_SVC_UUID        "7a0b0001-6f1e-4c2a-9c3b-2b7a4c1d0e01"
#define BLE_CHR_STATUS_UUID "7a0b0002-6f1e-4c2a-9c3b-2b7a4c1d0e01"   // notify: enc(BoilerStatus) + FrameTrailer
#define BLE_CHR_CTRL_UUID   "7a0b0003-6f1e-4c2a-9c3b-2b7a4c1d0e01"   // write:  enc(BoilerControl) + FrameTrailer
#define BLE_CHR_CMD_UUID    "7a0b0004-6f1e-4c2a-9c3b-2b7a4c1d0e01"   // write:  enc(1 byte command) + FrameTrailer
#define BLE_CHR_AUTH_UUID   "7a0b0005-6f1e-4c2a-9c3b-2b7a4c1d0e01"   // write/read: handshake

// ---- handshake messages on the AUTH characteristic ----
#define AUTH_NONCE_LEN   16
#define AUTH_PROOF_LEN   16
#define AUTH_HELLO       0x01   // display -> node : [0x01][version][nonceD 16]
#define AUTH_CHALLENGE   0x02   // node (read)     : [0x02][nonceN 16][proofN 16], proofN = HMAC(PSK, 'N'|ver|nonceD|nonceN)
#define AUTH_PROOF       0x03   // display -> node : [0x03][proofD 16],            proofD = HMAC(PSK, 'D'|ver|nonceN|nonceD)
                                // session key Ks  = HMAC(PSK, 'S'|ver|nonceD|nonceN)
#define AUTH_TIMEOUT_MS        5000   // known peer: time allowed to authenticate
#define AUTH_TIMEOUT_UNKNOWN_MS 2000  // unknown peer address: shorter, so a stranger cannot hold the slot

// ---- authenticated, encrypted frames ----
#define FRAME_TAG_LEN    8
#define FRAME_CTRL       0x10
#define FRAME_CMD        0x11
#define FRAME_STATUS     0x20

struct __attribute__((packed)) FrameTrailer {
  uint32_t counter;                 // strictly increasing per direction within a session
  uint8_t  tag[FRAME_TAG_LEN];      // AES-256-CCM tag over type (AAD) and payload
};

// status flags
#define BF_FLAME     (1 << 0)
#define BF_CH_ACTIVE (1 << 1)
#define BF_DHW_ACTIVE (1 << 2)
#define BF_FAULT     (1 << 3)
#define BF_DIAG      (1 << 4)
#define BF_OT_LINK   (1 << 5)   // OpenTherm responses arriving
#define BF_FALLBACK  (1 << 6)   // node running fixed flow setpoint (no control writes)
#define BF_SIMULATED (1 << 7)

// status flags2: what the node's own safety envelope is doing
#define BF2_FROST    (1 << 0)   // node frost protection active (water temperature too low)
#define BF2_CLAMPED  (1 << 1)   // requested flow setpoint was limited by the node
#define BF2_HOLD     (1 << 2)   // heating enable change is being delayed by the minimum on/off time

// control flags
#define CF_CH_ENABLE  (1 << 0)
#define CF_DHW_ENABLE (1 << 1)

// commands
#define CMD_RESET_FAULT 0x01

// Control frames outside this range are rejected by the node as nonsense, even when correctly
// authenticated. The working limits (clamp, minimum on/off times, frost guard) are in node_safety.h.
#define NODE_ACCEPT_MIN_C10  100
#define NODE_ACCEPT_MAX_C10  900

struct __attribute__((packed)) BoilerStatus {
  int16_t  flow_c10;        // 0.1 °C
  int16_t  return_c10;
  int16_t  dhw_c10;
  int16_t  outside_c10;     // INT16_MIN if boiler has no sensor
  uint16_t pressure_mbar;
  int16_t  ctrl_setpoint_c10; // what the node is currently asking the boiler for
  uint8_t  modulation;      // %
  uint8_t  flags;           // BF_*
  uint8_t  fault_flags;     // OT ID 5 high byte
  uint8_t  oem_fault;       // OT ID 5 low byte
  uint16_t oem_diag;        // OT ID 115
  uint16_t seq;             // increments per notify
  uint8_t  flags2;          // BF2_*
};

struct __attribute__((packed)) BoilerControl {
  int16_t ch_setpoint_c10;  // desired flow temperature, 0.1 °C
  int16_t dhw_setpoint_c10; // 0 = leave boiler default
  uint8_t flags;            // CF_*
};
