// BLE central for the CV-Boiler node (XIAO C6). NimBLE-Arduino 2.x.
//
// - Mutual authentication, then AES-256-CCM frames (see ble_protocol.h / ble_auth.h).
// - Node identity is pinned: after the first successful handshake only that address is accepted.
// - All NimBLE callbacks only stash data; st is updated from ble_link_loop() on the main task,
//   because LVGL and the shared state are single-writer.

#include "ble_link.h"
#include "state.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "ble_auth.h"
#include "ble_key.h"
#include "esp_random.h"
#include "evlog.h"

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *chrStatus = nullptr, *chrCtrl = nullptr, *chrCmd = nullptr, *chrAuth = nullptr;
static const NimBLEAdvertisedDevice *target = nullptr;
static volatile bool haveTarget = false;
static volatile bool connectedFlag = false;
static volatile bool statusDirty = false;
static BoilerStatus pending;
static uint32_t lastStatusMs = 0, lastScanMs = 0;
static volatile int lastDiscReason = 0;         // NimBLE reason of the most recent disconnect
#define DISC_REMOTE_TERMINATED (0x200 + 0x13)   // the peer closed the connection on purpose

// ---- session security ----
static const uint8_t PSK[32] = BLE_PSK;
static uint8_t Ks[32];
static volatile bool authed = false;
static uint32_t txCounter = 0, rxCounter = 0;

// ---- pinned node ----
static Preferences prefs;
static uint8_t pinned[6];
static volatile bool havePinned = false;
static bool everLinked = false;
static bool rediscover = true;         // true = fetch the node's services again; false = reuse what the client cached

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (haveTarget) return;
    if (!(dev->isAdvertisingService(NimBLEUUID(BLE_SVC_UUID)) || dev->getName() == BLE_DEVICE_NAME)) return;
    if (havePinned && memcmp(dev->getAddress().getVal(), pinned, 6) != 0) return;   // something else advertising as our node: ignore it
    elog("BLE found %s %s rssi %d", dev->getName().c_str(), dev->getAddress().toString().c_str(), (int)dev->getRSSI());
    target = dev;
    haveTarget = true;
    NimBLEDevice::getScan()->stop();
  }
  void onScanEnd(const NimBLEScanResults &, int) override {}
};

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { connectedFlag = true; }
  void onDisconnect(NimBLEClient *, int reason) override {
    connectedFlag = false;
    authed = false;
    lastDiscReason = reason;
    memset(Ks, 0, sizeof(Ks));
    elog("BLE disconnected (%d)", reason);
  }
};

static void notify_cb(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  // only frames encrypted with this session's key and carrying a fresh counter are accepted
  BoilerStatus s;
  if (!authed || !ba_open(Ks, FRAME_STATUS, data, len, sizeof(BoilerStatus), &rxCounter, (uint8_t *)&s)) return;
  pending = s;
  statusDirty = true;
}

// Mutual challenge-response. Returns true only if the node proved it holds the pre-shared key.
static bool handshake() {
  uint8_t nonceD[AUTH_NONCE_LEN], hello[2 + AUTH_NONCE_LEN];
  esp_fill_random(nonceD, sizeof(nonceD));
  hello[0] = AUTH_HELLO;
  hello[1] = BLE_PROTO_VERSION;
  memcpy(hello + 2, nonceD, AUTH_NONCE_LEN);
  if (!chrAuth->writeValue(hello, sizeof(hello), true)) {
    // A node that speaks another protocol revision hangs up on the hello. A link that merely dropped
    // (radio busy, out of range) looks different: the reason is a timeout, not a remote close.
    bool nodeHungUp = lastDiscReason == DISC_REMOTE_TERMINATED;
    st.bleVersionMismatch = nodeHungUp;
    elog("%s", nodeHungUp ? "auth: node closed the link on hello (node and display firmware do not match?)"
                          : "auth: link dropped during hello, will retry");
    return false;
  }

  NimBLEAttValue ch = chrAuth->readValue();
  if (ch.size() != 1 + AUTH_NONCE_LEN + AUTH_PROOF_LEN || ch.data()[0] != AUTH_CHALLENGE) {
    elog("auth: bad challenge (node and display firmware do not match?)");
    st.bleVersionMismatch = true;
    return false;
  }
  st.bleVersionMismatch = false;
  const uint8_t *nonceN = ch.data() + 1;
  const uint8_t *proofN = ch.data() + 1 + AUTH_NONCE_LEN;
  uint8_t h[32];
  ba_derive(PSK, 'N', nonceD, nonceN, h);
  if (!ba_equal(h, proofN, AUTH_PROOF_LEN)) {
    elog("auth: NODE FAILED AUTHENTICATION (wrong key or impostor)");
    st.bleAuthFailed = true;
    return false;
  }

  uint8_t proof[1 + AUTH_PROOF_LEN];
  ba_derive(PSK, 'D', nonceN, nonceD, h);
  proof[0] = AUTH_PROOF;
  memcpy(proof + 1, h, AUTH_PROOF_LEN);
  ba_derive(PSK, 'S', nonceD, nonceN, Ks);
  txCounter = rxCounter = 0;
  if (!chrAuth->writeValue(proof, sizeof(proof), true)) { elog("auth: proof write failed"); return false; }
  authed = true;
  st.bleAuthFailed = false;
  return true;
}

static ScanCB scanCB;
static ClientCB clientCB;

static void start_scan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->start(10000, false, true);
  lastScanMs = millis();
}

static bool do_connect() {
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
    client->setConnectionParams(24, 48, 0, 400);
    client->setConnectTimeout(5000);
  }
  uint8_t addr[6];
  memcpy(addr, target->getAddress().getVal(), 6);
  lastDiscReason = 0;
  // NimBLE keeps the service table from the previous connection. Reusing it (second argument false)
  // skips discovery and makes a reconnect noticeably quicker. After any failure we discover afresh.
  bool fresh = rediscover;
  rediscover = true;                      // assume the worst until this attempt has fully succeeded
  if (!client->connect(target, fresh)) {
    elog("BLE connect failed");
    return false;
  }
  NimBLERemoteService *svc = client->getService(BLE_SVC_UUID);
  if (!svc) { elog("BLE service missing"); client->disconnect(); return false; }
  chrStatus = svc->getCharacteristic(BLE_CHR_STATUS_UUID);
  chrCtrl   = svc->getCharacteristic(BLE_CHR_CTRL_UUID);
  chrCmd    = svc->getCharacteristic(BLE_CHR_CMD_UUID);
  chrAuth   = svc->getCharacteristic(BLE_CHR_AUTH_UUID);
  if (!chrStatus || !chrCtrl || !chrCmd || !chrAuth) { elog("BLE chars missing"); client->disconnect(); return false; }
  if (!handshake()) { client->disconnect(); return false; }
  if (!chrStatus->subscribe(true, notify_cb)) { elog("BLE subscribe failed"); client->disconnect(); return false; }
  lastStatusMs = millis();

  if (!havePinned) {                         // first authenticated node becomes THE node
    memcpy(pinned, addr, 6);
    havePinned = true;
    prefs.begin("thermo", false); prefs.putBytes("node", pinned, 6); prefs.end();
    elog("BLE node address pinned");
  }
  if (everLinked) st.bleReconnects++;
  everLinked = true;
  rediscover = false;                     // this table is known good: reuse it next time
  elog("BLE linked to boiler node (authenticated, encrypted session)");
  return true;
}

void ble_link_init() {
  prefs.begin("thermo", true);
  havePinned = prefs.isKey("node") && prefs.getBytesLength("node") == 6 && prefs.getBytes("node", pinned, 6) == 6;
  prefs.end();
  st.nodePinned = havePinned;
  NimBLEDevice::init("CV-Display");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  start_scan();
}

void ble_link_forget_node() {
  prefs.begin("thermo", false); prefs.remove("node"); prefs.end();
  havePinned = false;
  rediscover = true;                      // a different node may have a different table
  st.nodePinned = false;
  st.bleAuthFailed = false;
  if (client && connectedFlag) client->disconnect();
  elog("BLE pinned node forgotten; the next node that knows the key will be pinned");
}

void ble_link_loop() {
  uint32_t now = millis();

  if (!connectedFlag) {
    if (haveTarget) {
      bool ok = do_connect();
      haveTarget = false;
      target = nullptr;
      if (!ok) start_scan();
    } else if (!NimBLEDevice::getScan()->isScanning() && now - lastScanMs > 3000) {
      start_scan();
    }
  } else if (now - lastStatusMs > 20000) {
    // notifies stopped: drop and reconnect
    elog("BLE status timeout");
    lastStatusMs = now;
    client->disconnect();
  }

  if (statusDirty) {
    statusDirty = false;
    lastStatusMs = now;
    BoilerStatus s;
    noInterrupts(); s = pending; interrupts();
    st.flow        = s.flow_c10 / 10.0f;
    st.ret         = s.return_c10 / 10.0f;
    st.dhw         = s.dhw_c10 / 10.0f;
    st.pressure    = s.pressure_mbar / 1000.0f;
    st.modulation  = s.modulation;
    st.flame       = s.flags & BF_FLAME;
    st.boilerLinked = (s.flags & BF_OT_LINK) != 0;
    st.boilerFallback = (s.flags & BF_FALLBACK) != 0;
    st.boilerSimulated = (s.flags & BF_SIMULATED) != 0;
    st.boilerFault = (s.flags & (BF_FAULT | BF_DIAG)) != 0;
    st.nodeFrost   = (s.flags2 & BF2_FROST) != 0;
    st.nodeClamped = (s.flags2 & BF2_CLAMPED) != 0;
    st.nodeHold    = (s.flags2 & BF2_HOLD) != 0;
    if (s.outside_c10 != INT16_MIN) { st.outside = s.outside_c10 / 10.0f; st.outsideOk = true; st.outsideMs = now; }
    if (s.flags & BF_FAULT) snprintf(st.lastFault, sizeof(st.lastFault), "E.%02u", s.oem_fault);
    else if (s.flags & BF_DIAG) snprintf(st.lastFault, sizeof(st.lastFault), "H.%u", s.oem_diag);
    else snprintf(st.lastFault, sizeof(st.lastFault), "none");
    if (client) st.bleRssi = client->getRssi();
  }
  st.bleLinked = connectedFlag && authed;
  st.nodePinned = havePinned;
}

void ble_link_send_control(const BoilerControl &c) {
  if (!connectedFlag || !authed || !chrCtrl) return;
  uint8_t frame[sizeof(BoilerControl) + sizeof(FrameTrailer)];
  memcpy(frame, &c, sizeof(c));
  size_t n = ba_seal(Ks, FRAME_CTRL, ++txCounter, frame, sizeof(c));
  if (n) chrCtrl->writeValue(frame, n, false);
}

void ble_link_send_cmd(uint8_t cmd) {
  if (!connectedFlag || !authed || !chrCmd) return;
  uint8_t frame[1 + sizeof(FrameTrailer)];
  frame[0] = cmd;
  size_t n = ba_seal(Ks, FRAME_CMD, ++txCounter, frame, 1);
  if (n) chrCmd->writeValue(frame, n, true);
}
