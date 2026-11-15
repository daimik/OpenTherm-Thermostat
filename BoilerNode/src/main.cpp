// XIAO ESP32-C6 boiler node
//
// BLE peripheral for the thermostat display, OpenTherm master for the boiler.
// The node is the last line of defence: whatever arrives over BLE, even correctly authenticated,
// passes through a safety envelope here before it reaches the boiler.
//
//   - mutual authentication + AES-256-CCM frames (see ble_protocol.h)
//   - flow setpoint clamp, nonsense rejection, minimum on/off times
//   - own frost protection from water temperature
//   - 10 min fallback to a fixed flow temperature when no display is in control
//   - task watchdog
//   - optional read-only status page over Wi-Fi (web.cpp), built in only when node_secrets.h exists;
//     it runs in its own task and nothing in this file waits for it

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "ble_protocol.h"
#include "ble_auth.h"
#include "ble_key.h"
#include "node_safety.h"
#include "nodelog.h"
#include "node_web.h"

#define PIN_LED        15        // XIAO C6 user LED, active low
#define PIN_RF_SW_EN   3         // XIAO C6: low = antenna switch powered
#define PIN_RF_SEL     14        // XIAO C6: low = ceramic, high = external U.FL
#define USE_EXT_ANTENNA 1
// OpenTherm adapter wiring, as it runs on the installed hardware:
//   adapter IN  -> D1 / GPIO1: the node READS the boiler's answers here (needs an interrupt-capable pin)
//   adapter OUT -> D2 / GPIO2: the node SENDS its requests here
//   adapter VCC -> 3V3 (never 5 V: these pins are not 5 V tolerant), adapter GND -> GND
// The adapter's labels are written from the microcontroller's side: IN goes to the controller's input,
// OUT comes from the controller's output. The OpenTherm library documentation describes the same two
// lines by signal direction ("inPin: connected to the adapter output"), which reads like the opposite.
#ifndef PIN_OT_RX
#define PIN_OT_RX      1         // D1: the node reads here. Wired to the adapter pin labelled IN
#endif
#ifndef PIN_OT_TX
#define PIN_OT_TX      2         // D2: the node sends here. Wired to the adapter pin labelled OUT
#endif
#define NOTIFY_MS      5000
#if BOILER_SIMULATED
#define NODE_BUILD "simulated boiler"
#elif PIN_OT_RX == 2
#define NODE_BUILD "real boiler, pins swapped"
#else
#define NODE_BUILD "real boiler"
#endif
#define WDT_TIMEOUT_MS 30000
#define OT_LINK_GRACE  5         // this many status exchanges in a row must fail before the boiler counts as gone

static BoilerStatus  status = {};
static BoilerControl control = { 450, 0, CF_CH_ENABLE | CF_DHW_ENABLE };
static uint32_t lastControlMs = 0;
static bool     everControlled = false;
static volatile bool resetRequested = false;

static NimBLEServer *server;
static NimBLECharacteristic *chrStatus;
static volatile bool connected = false;

// ---- session security ----
static const uint8_t PSK[32] = BLE_PSK;
static uint8_t  nonceD[AUTH_NONCE_LEN], nonceN[AUTH_NONCE_LEN], Ks[32];
static uint8_t  authState = 0;          // 0 idle, 1 challenge issued
static volatile bool authed = false;
static uint32_t rxCounter = 0, txCounter = 0;
static uint32_t connectMs = 0, authTimeoutMs = AUTH_TIMEOUT_MS;
static uint16_t connHandle = 0;
static uint32_t lastNotifyMs = 0;

// ---- peer memory and back-off ----
static Preferences prefs;
static uint8_t  knownPeer[6];
static bool     haveKnownPeer = false;
static uint8_t  curPeer[6];
static volatile uint32_t failCount = 0;       // consecutive connections that ended without authentication
static volatile uint32_t advResumeMs = 0;

// ---- safety envelope (logic in node_safety.h, unit-tested on the PC) ----
static const SafetyParams safetyParams = {};
static SafetyState safety;
static uint32_t rejectedFrames = 0;

// ---- counters shown on the status page ----
static volatile uint32_t ctrlFrames = 0, bleSessions = 0, bleDrops = 0;
static volatile int lastDropReason = 0;
static uint32_t flameStarts = 0, flameOnMs = 0;

static void session_reset() {
  authed = false;
  authState = 0;
  rxCounter = txCounter = 0;
  memset(Ks, 0, sizeof(Ks));
}

// ---------------- boiler backend ----------------
#if BOILER_SIMULATED
static float simFlow = 30.0f, simRet = 28.0f;
static void boiler_tick(float dt, int16_t sp_c10, bool chEnable) {
  float sp = sp_c10 / 10.0f;
  bool demand = chEnable && simFlow < sp - 1.0f;
  float mod = 0;
  if (demand) {
    mod = constrain((sp - simFlow) * 15.0f, 20.0f, 100.0f);
    simFlow += (mod / 100.0f) * 1.2f * dt;           // heat up
  } else {
    simFlow -= 0.15f * dt;                            // cool down
    if (simFlow < 22) simFlow = 22;
  }
  simRet += ((simFlow - 12.0f) - simRet) * 0.1f * dt;
  status.flow_c10 = simFlow * 10;
  status.return_c10 = simRet * 10;
  status.dhw_c10 = 600;
  status.outside_c10 = INT16_MIN;
  status.pressure_mbar = 1800;
  status.modulation = (uint8_t)mod;
  status.flags = BF_OT_LINK | BF_SIMULATED | (demand ? (BF_FLAME | BF_CH_ACTIVE) : 0);
  if (resetRequested) { status.fault_flags = 0; status.oem_fault = 0; status.oem_diag = 0; resetRequested = false; }
}
static uint32_t boiler_errors() { return 0; }
static void boiler_diag(char *out, size_t len) { snprintf(out, len, "simulated"); }
static void boiler_view(NodeView &v) {
  v.simulated = true;
  snprintf(v.otLast, sizeof(v.otLast), "simulated");
  v.otOk = v.otTimeout = v.otInvalid = v.otReadErr = v.otEdges = 0;
  v.otRxLevel = 0; v.pinRx = PIN_OT_RX; v.pinTx = PIN_OT_TX;
  v.otMissing = 0;
}
static const char *boiler_last() { return "simulated"; }
#else
#include <OpenTherm.h>
// Real boiler. Written against the OpenTherm Library documentation (ihormelnyk/opentherm_library):
//   - sendRequest() blocks for one exchange, up to 1 s when the boiler does not answer
//   - its return value is not enough to tell success: check getLastResponseStatus()
//   - the convenience getters return 0 for an invalid answer, which this node must never mistake
//     for "the water is at 0 °C". Every reading therefore goes through ot_read() and a value is
//     only replaced when the exchange succeeded.
static OpenTherm ot(PIN_OT_RX, PIN_OT_TX);
static volatile uint32_t otEdges = 0;        // level changes seen on the receive pin since boot
static void IRAM_ATTR ot_isr() { otEdges = otEdges + 1; ot.handleInterrupt(); }
static const char *otLast = "none";          // outcome of the most recent status exchange

// Commissioning aid, printed in the status line:
//   ot=timeout edges stays at 0  -> nothing ever arrives: the boiler is not answering, or the receive pin
//                                   (D1) is not on the adapter's IN (the two signal wires crossed, or a wire off)
//   ot=invalid, edges climbing   -> something arrives but is not a valid frame: noise, or a poor connection
//   ot=ok                        -> the boiler is answering
static void boiler_diag(char *out, size_t len) {
  snprintf(out, len, "ot=%s edges=%lu rx=%d pins=%d/%d", otLast, (unsigned long)otEdges, digitalRead(PIN_OT_RX), PIN_OT_RX, PIN_OT_TX);
}
static uint32_t otTick = 0;
static uint32_t otReadErrors = 0;
static uint32_t otOk = 0, otTimeout = 0, otInvalid = 0;      // outcome of the once-a-second status exchange
static uint8_t  otFailStreak = 0;
static uint8_t  otMissing = 0;                                // optional readings the boiler does not provide, bit per OPT_*
static uint32_t boiler_errors() { return otReadErrors; }
static const char *boiler_last() { return otLast; }
static void boiler_view(NodeView &v) {
  v.simulated = false;
  snprintf(v.otLast, sizeof(v.otLast), "%s", otLast);
  v.otOk = otOk; v.otTimeout = otTimeout; v.otInvalid = otInvalid; v.otReadErr = otReadErrors; v.otEdges = otEdges;
  v.otRxLevel = (uint8_t)digitalRead(PIN_OT_RX); v.pinRx = PIN_OT_RX; v.pinTx = PIN_OT_TX;
  v.otMissing = otMissing;
}

// Optional readings. A boiler may answer "I do not have that": a well-formed frame with message type
// DATA-INVALID (6) or UNKNOWN-DATAID (7). That is an answer, not a transmission error, so it is not
// counted as one. After three such answers in a row the node stops asking for that value until restart.
enum { OPT_DHW, OPT_PRESSURE, OPT_OUTSIDE, OPT_COUNT };
static const char *const optName[OPT_COUNT] = { "hot water temperature (ID 26)", "water pressure (ID 18)", "outside temperature (ID 27)" };
static uint8_t optStrikes[OPT_COUNT];
static bool    otDeclined = false;           // the latest failed reading was the boiler declining

static bool ot_read(OpenThermMessageID id, unsigned long *resp) {
  otDeclined = false;
  unsigned long r = ot.sendRequest(OpenTherm::buildRequest(OpenThermMessageType::READ_DATA, id, 0));
  OpenThermResponseStatus rs = ot.getLastResponseStatus();
  if (rs == OpenThermResponseStatus::SUCCESS && OpenTherm::isValidResponse(r)) { *resp = r; return true; }
  uint8_t type = (r >> 28) & 7;
  bool wellFormed = rs != OpenThermResponseStatus::TIMEOUT && r != 0 && __builtin_parityl(r & 0xFFFFFFFFUL) == 0;
  if (wellFormed && (type == 6 || type == 7)) otDeclined = true;
  else otReadErrors++;
  return false;
}
static bool opt_wanted(int k) { return !(otMissing & (1 << k)); }
static void opt_result(int k, bool ok) {
  if (ok) { optStrikes[k] = 0; return; }
  if (!otDeclined) return;                   // a lost frame says nothing about what the boiler supports
  if (++optStrikes[k] >= 3) {
    otMissing |= (1 << k);
    nlog("boiler does not provide %s: not asked again until restart", optName[k]);
  }
}
static bool ot_read_temp(OpenThermMessageID id, int16_t *out_c10) {
  unsigned long r;
  if (!ot_read(id, &r)) return false;
  float v = ot.getFloat(r);
  if (v < -40.0f || v > 130.0f) return false;          // not a temperature this system can have
  *out_c10 = (int16_t)lroundf(v * 10);
  return true;
}

static void boiler_tick(float, int16_t sp_c10, bool chEnable) {
  unsigned long r;
  int16_t t;                               // readings land here first: the status struct is packed
  otTick++;

  // 1. Status exchange, every second. OpenTherm requires the master to talk at least once per second,
  //    and this is also what tells us whether the boiler is there at all.
  unsigned long resp = ot.setBoilerStatus(chEnable, (control.flags & CF_DHW_ENABLE) != 0);
  OpenThermResponseStatus rs = ot.getLastResponseStatus();
  bool link = rs == OpenThermResponseStatus::SUCCESS;
  otLast = link ? "ok" : (rs == OpenThermResponseStatus::TIMEOUT ? "timeout" : (rs == OpenThermResponseStatus::INVALID ? "invalid" : "none"));
  if (link) { otOk++; otFailStreak = 0; }
  else {
    if (rs == OpenThermResponseStatus::TIMEOUT) otTimeout++; else otInvalid++;
    if (otFailStreak < 255) otFailStreak++;
    // One missed exchange is not a lost boiler. Radio activity can cost a frame now and then; the boiler
    // itself only gives up after about a minute of silence. Until OT_LINK_GRACE exchanges in a row have
    // failed, the link flag and the last known burner state stay as they were (at most a few seconds old).
    if (otFailStreak < OT_LINK_GRACE && (status.flags & BF_OT_LINK)) {
      status.flags &= (BF_OT_LINK | BF_FLAME | BF_CH_ACTIVE | BF_DHW_ACTIVE | BF_FAULT | BF_DIAG);
      return;
    }
  }
  uint8_t f = link ? BF_OT_LINK : 0;
  if (link) {
    if (ot.isFlameOn(resp))               f |= BF_FLAME;
    if (ot.isCentralHeatingActive(resp))  f |= BF_CH_ACTIVE;
    if (ot.isHotWaterActive(resp))        f |= BF_DHW_ACTIVE;
    if (ot.isFault(resp))                 f |= BF_FAULT;
    if (ot.isDiagnostic(resp))            f |= BF_DIAG;
  }
  status.flags = f;
  if (!link) return;                       // boiler silent: keep the last readings, the flag says they are stale

  // 2. Control setpoint, every second.
  ot.setBoilerTemperature(sp_c10 / 10.0f);

  // 3. One fast reading per second in rotation (flow, return, modulation), so a tick stays well under
  //    a second even with the mandatory gaps between exchanges.
  switch (otTick % 3) {
    case 0: if (ot_read_temp(OpenThermMessageID::Tboiler, &t)) status.flow_c10 = t; break;
    case 1: if (ot_read_temp(OpenThermMessageID::Tret, &t)) status.return_c10 = t; break;
    case 2: if (ot_read(OpenThermMessageID::RelModLevel, &r)) { float m = ot.getFloat(r); status.modulation = (uint8_t)constrain(m, 0.0f, 100.0f); } break;
  }

  // 4. One slow reading every 10 s in rotation (hot water, pressure, outdoor sensor, hot-water setpoint).
  if (otTick % 10 == 5) {
    switch ((otTick / 10) % 4) {
      case 0:
        if (opt_wanted(OPT_DHW)) { bool ok = ot_read_temp(OpenThermMessageID::Tdhw, &t); if (ok) status.dhw_c10 = t; opt_result(OPT_DHW, ok); }
        break;
      case 1:
        if (opt_wanted(OPT_PRESSURE)) {
          bool ok = ot_read(OpenThermMessageID::CHPressure, &r);
          if (ok) { float p = ot.getFloat(r); if (p >= 0.0f && p < 6.0f) status.pressure_mbar = (uint16_t)lroundf(p * 1000); }
          opt_result(OPT_PRESSURE, ok);
        }
        break;
      case 2:                                 // only boilers with an outdoor sensor answer this one
        if (opt_wanted(OPT_OUTSIDE)) {
          bool ok = ot_read_temp(OpenThermMessageID::Toutside, &t);
          status.outside_c10 = (ok && t > -600 && t < 600) ? t : INT16_MIN;
          opt_result(OPT_OUTSIDE, ok);
        }
        break;
      case 3: if (control.dhw_setpoint_c10 > 0) ot.setDHWSetpoint(control.dhw_setpoint_c10 / 10.0f); break;
    }
  }

  // 5. Fault details, only while the boiler signals a fault or a diagnostic event.
  if (f & BF_FAULT) {
    if (ot_read(OpenThermMessageID::ASFflags, &r)) { status.fault_flags = (r >> 8) & 0xFF; status.oem_fault = r & 0xFF; }
  } else if (f & BF_DIAG) {
    if (ot_read((OpenThermMessageID)115, &r)) status.oem_diag = r & 0xFFFF;      // ID 115: OEM diagnostic code
  } else {
    status.fault_flags = 0; status.oem_fault = 0; status.oem_diag = 0;
  }

  // 6. Lock-out reset on request: Remote Request (ID 4), command 1 "BLOR" in the high byte.
  if (resetRequested) {
    // by number, not by name: the enumerator for ID 4 has different names in different library releases
    ot.sendRequest(OpenTherm::buildRequest(OpenThermMessageType::WRITE_DATA, (OpenThermMessageID)4, 1 << 8));
    resetRequested = false;
  }
}
#endif

// ---------------- BLE ----------------
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &info) override {
    session_reset();
    connected = true;
    connectMs = millis();
    connHandle = info.getConnHandle();
    memcpy(curPeer, info.getAddress().getVal(), 6);
    bool known = haveKnownPeer && memcmp(curPeer, knownPeer, 6) == 0;
    authTimeoutMs = known ? AUTH_TIMEOUT_MS : AUTH_TIMEOUT_UNKNOWN_MS;
    nlog("BLE connected %s (%s peer), waiting for auth", info.getAddress().toString().c_str(), known ? "known" : "unknown");
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    bool wasAuthed = authed;
    connected = false;
    session_reset();
    chrStatus->setValue((uint8_t *)"", 0);
    uint32_t delayMs = 0;
    if (wasAuthed) {
      failCount = 0;
    } else {
      failCount = failCount + 1;
      // After 5 unauthenticated connections in a row, slow the stranger down. Capped at 30 s so the
      // real display is never locked out for long.
      if (failCount >= 5) { delayMs = 5000UL * (failCount - 4); if (delayMs > 30000) delayMs = 30000; }
    }
    advResumeMs = millis() + delayMs;
    bleDrops = bleDrops + 1;
    lastDropReason = reason;
    nlog("BLE disconnected (%d), advertising again in %lu ms, unauth streak %lu", reason, (unsigned long)delayMs, (unsigned long)failCount);
  }
};

class AuthCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    const uint8_t *d = v.data();
    if (v.size() == 2 + AUTH_NONCE_LEN && d[0] == AUTH_HELLO) {
      if (d[1] != BLE_PROTO_VERSION) {
        nlog("auth: display speaks protocol %u, this node %u: flash both devices from the same set", d[1], BLE_PROTO_VERSION);
        server->disconnect(info.getConnHandle());
        return;
      }
      memcpy(nonceD, d + 2, AUTH_NONCE_LEN);
      esp_fill_random(nonceN, AUTH_NONCE_LEN);
      uint8_t h[32], resp[1 + AUTH_NONCE_LEN + AUTH_PROOF_LEN];
      ba_derive(PSK, 'N', nonceD, nonceN, h);
      resp[0] = AUTH_CHALLENGE;
      memcpy(resp + 1, nonceN, AUTH_NONCE_LEN);
      memcpy(resp + 1 + AUTH_NONCE_LEN, h, AUTH_PROOF_LEN);
      c->setValue(resp, sizeof(resp));
      authed = false;
      authState = 1;
      return;
    }
    if (v.size() == 1 + AUTH_PROOF_LEN && d[0] == AUTH_PROOF && authState == 1) {
      uint8_t h[32];
      ba_derive(PSK, 'D', nonceN, nonceD, h);
      if (ba_equal(h, d + 1, AUTH_PROOF_LEN)) {
        ba_derive(PSK, 'S', nonceD, nonceN, Ks);
        rxCounter = txCounter = 0;
        authState = 0;
        authed = true;
        failCount = 0;
        bleSessions = bleSessions + 1;
        lastNotifyMs = millis() - NOTIFY_MS + 800;     // first status shortly after the display subscribes
        c->setValue((uint8_t *)"", 0);
        if (!haveKnownPeer || memcmp(curPeer, knownPeer, 6) != 0) {
          memcpy(knownPeer, curPeer, 6);
          haveKnownPeer = true;
          prefs.begin("node", false); prefs.putBytes("peer", knownPeer, 6); prefs.end();
          nlog("auth OK, peer address remembered");
        } else {
          nlog("auth OK, session open");
        }
        return;
      }
      nlog("auth FAILED: wrong proof, dropping connection");
      server->disconnect(info.getConnHandle());
      return;
    }
    nlog("auth: malformed message, dropping connection");
    server->disconnect(info.getConnHandle());
  }
};

static bool control_plausible(const BoilerControl &c) {
  if (c.ch_setpoint_c10 < NODE_ACCEPT_MIN_C10 || c.ch_setpoint_c10 > NODE_ACCEPT_MAX_C10) return false;
  if (c.dhw_setpoint_c10 != 0 && (c.dhw_setpoint_c10 < 300 || c.dhw_setpoint_c10 > 650)) return false;
  if (c.flags & ~(CF_CH_ENABLE | CF_DHW_ENABLE)) return false;
  return true;
}

class CtrlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    BoilerControl in;
    if (!authed || !ba_open(Ks, FRAME_CTRL, v.data(), v.size(), sizeof(BoilerControl), &rxCounter, (uint8_t *)&in)) {
      rejectedFrames++;
      nlog("ctrl: rejected (not authenticated / bad tag / replay)");
      return;
    }
    if (!control_plausible(in)) {
      rejectedFrames++;
      nlog("ctrl: authenticated but out of range (ch=%d dhw=%d flags=0x%02x), ignored", in.ch_setpoint_c10, in.dhw_setpoint_c10, in.flags);
      return;
    }
    control = in;
    lastControlMs = millis();
    everControlled = true;
    ctrlFrames = ctrlFrames + 1;
    // A frame arrives at least every half minute. Only a real change is worth a line in the log:
    // heating or hot water switched, or the flow setpoint moved by 2 degrees or more since the last line.
    static BoilerControl logged = { 0, 0, 0xFF };
    int16_t moved = in.ch_setpoint_c10 - logged.ch_setpoint_c10;
    if (in.flags != logged.flags || in.dhw_setpoint_c10 != logged.dhw_setpoint_c10 || moved >= 20 || moved <= -20) {
      logged = in;
      nlog("ctrl: heating %s, flow %.1f, hot water %s", (in.flags & CF_CH_ENABLE) ? "on" : "off", in.ch_setpoint_c10 / 10.0,
           (in.flags & CF_DHW_ENABLE) ? "on" : "off");
    }
  }
};

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    uint8_t cmd;
    if (!authed || !ba_open(Ks, FRAME_CMD, v.data(), v.size(), 1, &rxCounter, &cmd)) {
      rejectedFrames++;
      nlog("cmd: rejected (not authenticated / bad tag / replay)");
      return;
    }
    if (cmd == CMD_RESET_FAULT) { resetRequested = true; nlog("cmd: reset boiler fault"); }
  }
};

static void ble_init() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(false);               // re-advertising is paced from loop()
  NimBLEService *svc = server->createService(BLE_SVC_UUID);
  chrStatus = svc->createCharacteristic(BLE_CHR_STATUS_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *chrAuth = svc->createCharacteristic(BLE_CHR_AUTH_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chrAuth->setCallbacks(new AuthCB());
  NimBLECharacteristic *chrCtrl = svc->createCharacteristic(BLE_CHR_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrCtrl->setCallbacks(new CtrlCB());
  NimBLECharacteristic *chrCmd = svc->createCharacteristic(BLE_CHR_CMD_UUID, NIMBLE_PROPERTY::WRITE);
  chrCmd->setCallbacks(new CmdCB());
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setName(BLE_DEVICE_NAME);
  adv->enableScanResponse(true);
  adv->start();
  nlog("BLE advertising as %s, addr %s", BLE_DEVICE_NAME, NimBLEDevice::getAddress().toString().c_str());
}

// ---------------- event log and status page ----------------
static void fmt_dur(uint32_t ms, char *out, size_t len) {
  uint32_t s = ms / 1000;
  if (s >= 3600)    snprintf(out, len, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
  else if (s >= 60) snprintf(out, len, "%lu min %02lu s", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else              snprintf(out, len, "%lu s", (unsigned long)s);
}

// Called once a second after the boiler exchange: writes one log line for every state that changed.
// Everything starts as "off", so the first second after boot records what is on.
static void log_changes(const SafetyOut &out, const SafetyIn &in, uint32_t now) {
  static uint8_t pf = 0;
  static bool pCh = false, pFallback = false, pFrost = false, pHold = false, pClamped = false;
  static uint8_t pFault = 0, pFaultFlags = 0;
  static uint16_t pDiag = 0;
  static uint32_t flameSince = 0, dhwSince = 0, lastTickMs = 0;
  static bool everLinked = false, saidSilent = false;
  uint8_t f = status.flags, changed = f ^ pf;
  float flow = status.flow_c10 / 10.0f, ret = status.return_c10 / 10.0f;
  char d[24];

  if (changed & BF_OT_LINK) {
    if (f & BF_SIMULATED)     nlog("simulated boiler running (no OpenTherm in this build)");
    else if (f & BF_OT_LINK)  nlog("boiler link up: OpenTherm answering");
    else                      nlog("boiler link LOST: %d exchanges without a valid answer (last: %s)", OT_LINK_GRACE, boiler_last());
  }
  if (f & BF_OT_LINK) everLinked = true;
  if (!everLinked && !saidSilent && now > 15000) {
    saidSilent = true;
    nlog("boiler not answering since boot (last: %s): check adapter wiring, IN to D1, OUT to D2", boiler_last());
  }
  if (changed & BF_FLAME) {
    if (f & BF_FLAME) { flameSince = now; flameStarts++; nlog("flame on: flow %.1f, return %.1f", flow, ret); }
    else              { fmt_dur(now - flameSince, d, sizeof(d)); nlog("flame off after %s: flow %.1f, return %.1f", d, flow, ret); }
  }
  if ((f & BF_FLAME) && (pf & BF_FLAME)) flameOnMs += now - lastTickMs;
  if (changed & BF_DHW_ACTIVE) {
    if (f & BF_DHW_ACTIVE) { dhwSince = now; nlog("hot water demand started"); }
    else                   { fmt_dur(now - dhwSince, d, sizeof(d)); nlog("hot water demand ended after %s", d); }
  }
  if (changed & BF_FAULT) nlog("%s", (f & BF_FAULT) ? "BOILER FAULT reported" : "boiler fault cleared");
  if ((f & BF_FAULT) && (status.oem_fault != pFault || status.fault_flags != pFaultFlags) && (status.oem_fault || status.fault_flags))
    nlog("boiler fault code E.%02u, flags 0x%02x", status.oem_fault, status.fault_flags);
  if ((f & BF_DIAG) && status.oem_diag != pDiag && status.oem_diag) nlog("boiler diagnostic code H.%u", status.oem_diag);

  if (out.ch != pCh) {
    if (out.ch) nlog("heating enabled, flow setpoint %.1f", out.flow_c10 / 10.0);
    else        nlog("heating disabled");
  }
  if (out.fallback != pFallback) {
    if (!out.fallback)        nlog("display in control, fallback ended");
    else if (in.haveControl)  nlog("FALLBACK: no control frame for %lu min, holding %.1f", (unsigned long)(in.controlAgeMs / 60000), safetyParams.fallbackFlow_c10 / 10.0);
    else                      nlog("FALLBACK: no display in control since boot, holding %.1f", safetyParams.fallbackFlow_c10 / 10.0);
  }
  if (out.frost != pFrost) {
    if (out.frost) nlog("FROST GUARD on: coldest water %.1f", in.return_c10 / 10.0);
    else           nlog("frost guard off");
  }
  if (out.hold && !pHold) nlog("hold: switching heating %s is delayed by the minimum on/off time", out.ch ? "off" : "on");
  if (out.clamped && !pClamped)
    nlog("setpoint limited: display asked %.1f, node allows %.1f to %.1f", in.reqFlow_c10 / 10.0, safetyParams.flowMin_c10 / 10.0, safetyParams.flowMax_c10 / 10.0);

  pf = f; pCh = out.ch; pFallback = out.fallback; pFrost = out.frost; pHold = out.hold; pClamped = out.clamped;
  pFault = status.oem_fault; pFaultFlags = status.fault_flags; pDiag = status.oem_diag;
  lastTickMs = now;
}

// Hands the status page a copy of everything it shows. Cheap, and a no-op without node_secrets.h.
static void publish_view(const SafetyOut &out, const SafetyIn &in) {
#if NODE_WEB
  static NodeView v;
  v.st = status;
  v.haveControl = in.haveControl;
  v.controlAgeMs = in.controlAgeMs;
  v.req = control;
  v.ctrlFrames = ctrlFrames;
  v.chOn = out.ch; v.fallback = out.fallback; v.frost = out.frost; v.clamped = out.clamped; v.hold = out.hold;
  v.bleState = !connected ? 0 : (authed ? 2 : 1);
  v.havePeer = haveKnownPeer;
  memcpy(v.peer, knownPeer, 6);
  v.sessions = bleSessions; v.drops = bleDrops; v.rejected = rejectedFrames; v.unauthStreak = failCount;
  v.lastDropReason = lastDropReason;
  boiler_view(v);
  v.flameStarts = flameStarts;
  v.flameOnS = flameOnMs / 1000;
  web_publish(v);
#else
  (void)out; (void)in;
#endif
}

// ---------------- main ----------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1200) delay(10);   // a monitor gets a moment to attach; a wall supply does not delay the boot
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  nlog("boiler node boot (%s), reset reason %d", NODE_BUILD, (int)esp_reset_reason());

  pinMode(PIN_RF_SW_EN, OUTPUT);
  digitalWrite(PIN_RF_SW_EN, LOW);
  delay(100);
  pinMode(PIN_RF_SEL, OUTPUT);
  digitalWrite(PIN_RF_SEL, USE_EXT_ANTENNA ? HIGH : LOW);
  nlog("antenna: %s", USE_EXT_ANTENNA ? "external" : "ceramic");

  prefs.begin("node", true);
  haveKnownPeer = prefs.getBytesLength("peer") == 6 && prefs.getBytes("peer", knownPeer, 6) == 6;
  prefs.end();

#if !BOILER_SIMULATED
  ot.begin(ot_isr);
#endif
  ble_init();
  web_begin(NODE_BUILD, (int)esp_reset_reason(), &safetyParams);   // does nothing without node_secrets.h

  // task watchdog: a hung loop resets the node, which then comes back in fallback
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = WDT_TIMEOUT_MS;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);
}

void loop() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  esp_task_wdt_reset();

  if (connected && !authed && now - connectMs > authTimeoutMs) {
    nlog("auth timeout, dropping connection");
    connectMs = now;
    server->disconnect(connHandle);
  }
  if (!connected && !NimBLEDevice::getAdvertising()->isAdvertising() && (int32_t)(now - advResumeMs) >= 0) {
    NimBLEDevice::startAdvertising();
  }

  if (now - lastTick >= 1000) {
    float dt = (now - lastTick) / 1000.0f;
    lastTick = now;

    SafetyIn in;
    in.nowMs = now;
    in.haveControl = everControlled;
    in.controlAgeMs = now - lastControlMs;
    in.reqCh = (control.flags & CF_CH_ENABLE) != 0;
    in.reqFlow_c10 = control.ch_setpoint_c10;
    // The frost guard may only act on temperatures that were really read. Until both flow and return
    // have been read once they are 0, which would look like freezing water.
    bool tempsKnown = status.flow_c10 != 0 && status.return_c10 != 0;
    in.otLink = (status.flags & BF_OT_LINK) != 0 && tempsKnown;
    in.return_c10 = status.return_c10 < status.flow_c10 ? status.return_c10 : status.flow_c10;   // coldest water we know of
    SafetyOut out = ns_apply(safety, in, safetyParams);

    boiler_tick(dt, out.flow_c10, out.ch);
    status.ctrl_setpoint_c10 = out.flow_c10;
    status.flags2 = (out.frost ? BF2_FROST : 0) | (out.clamped ? BF2_CLAMPED : 0) | (out.hold ? BF2_HOLD : 0);
    status.flags = (status.flags & ~BF_FALLBACK) | (out.fallback ? BF_FALLBACK : 0);

    digitalWrite(PIN_LED, (status.flags & BF_FLAME) ? LOW : HIGH);
    log_changes(out, in, now);
    publish_view(out, in);
  }

  if (now - lastNotifyMs >= NOTIFY_MS) {
    lastNotifyMs = now;
    status.seq++;
    if (connected && authed) {                       // status leaves the node only inside an authenticated session
      uint8_t frame[sizeof(BoilerStatus) + sizeof(FrameTrailer)];
      memcpy(frame, &status, sizeof(status));
      size_t n = ba_seal(Ks, FRAME_STATUS, ++txCounter, frame, sizeof(status));
      if (n) { chrStatus->setValue(frame, n); chrStatus->notify(); }
    }
    char diag[64];
    boiler_diag(diag, sizeof(diag));
    Serial.printf("st: flow=%.1f ret=%.1f mod=%u sp=%.1f en=%d flags=0x%02x f2=0x%02x rej=%lu oterr=%lu %s %s\n",
                  status.flow_c10 / 10.0, status.return_c10 / 10.0, status.modulation,
                  status.ctrl_setpoint_c10 / 10.0, safety.chOn, status.flags, status.flags2, (unsigned long)rejectedFrames,
                  (unsigned long)boiler_errors(), diag,
                  !connected ? "adv" : (authed ? "session" : "conn-unauth"));
  }
  delay(10);
}
