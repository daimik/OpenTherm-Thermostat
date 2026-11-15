// Wi-Fi (station), NTP -> RTC, and the name thermostat.local. Everything here is optional for heating:
// if Wi-Fi is down the thermostat keeps running from the RTC and the BLE link.

#include "net.h"
#include "state.h"
#include "clock.h"
#include "secrets.h"
#include "evlog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include "mbedtls/md.h"

static bool mdnsStarted = false, ntpSynced = false;
static uint32_t lastNtpMs = 0, lastDownMs = 0;

static const char *reason_text(uint8_t r) {
  switch (r) {
    case 2:   return "auth expired (AP did not answer, weak signal or MAC filter)";
    case 15:  return "4-way handshake timeout = WRONG PASSWORD (most likely)";
    case 201: return "NO AP FOUND = SSID not visible on 2.4 GHz (5 GHz only, hidden, or typo/case)";
    case 202: return "auth failed = wrong password or WPA3-only network";
    case 203: return "association failed (AP refused: client limit, MAC filter)";
    case 204: return "handshake timeout = wrong password";
    case 211: return "no AP with compatible security (WPA3-only / PMF required?)";
    default:  return "see esp_wifi_types.h wifi_err_reason_t";
  }
}

static char pwInfo[40] = "";

static void wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    const uint8_t *b = info.wifi_sta_disconnected.bssid;
    static uint8_t lastReason = 0; static uint32_t repeats = 0;       // the same reason every few seconds must not fill the log
    repeats = (r == lastReason) ? repeats + 1 : 0;
    lastReason = r;
    if (repeats < 3 || repeats % 20 == 0) {
      elog("Wi-Fi disconnect, reason %u: %s", r, reason_text(r));
      elog("   via AP %02x:%02x:%02x:%02x:%02x:%02x, %s", b[0], b[1], b[2], b[3], b[4], b[5], pwInfo);
    }
  } else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    elog("Wi-Fi associated, channel %u", info.wifi_sta_connected.channel);
  }
}

static const char *auth_text(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3 only";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
  }
}

// Diagnostic scan: shows what the radio can see. Not run at boot (it would freeze the display for
// four seconds); started in the background, once per boot, only when Wi-Fi has been down for 25 s.
static void scan_report(int n) {
  bool seen = false;
  elog("Wi-Fi scan: %d networks on 2.4 GHz", n);
  for (int i = 0; i < n; i++) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    if (match) seen = true;
    if (match || i < 8)
      elog("  %s%-24s %s  ch %2d  %4d dBm  %s", match ? "> " : "  ", WiFi.SSID(i).c_str(),
                    WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), auth_text(WiFi.encryptionType(i)));
  }
  if (!seen) elog("  \"%s\" NOT SEEN: no 2.4 GHz, hidden, out of range, or the name differs", WIFI_SSID);
  WiFi.scanDelete();
}

static void wifi_join() {
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

enum { DIAG_IDLE, DIAG_RUNNING, DIAG_DONE };
static uint8_t diag = DIAG_IDLE;

static uint32_t downSinceMs = 0;             // when Wi-Fi was last seen going down (0 = since boot)

// Called while Wi-Fi is down. Returns true while the diagnostic owns the radio.
static bool diag_scan(uint32_t now) {
  if (diag == DIAG_IDLE && now - downSinceMs > 25000) {
    WiFi.setAutoReconnect(false);            // a station that is busy connecting cannot scan
    WiFi.disconnect();
    WiFi.scanNetworks(true, true, false, 300);     // async: returns at once, the result is polled below
    diag = DIAG_RUNNING;
    elog("Wi-Fi down for 25 s, scanning to see why");
  }
  if (diag != DIAG_RUNNING) return false;
  int16_t n = WiFi.scanComplete();           // -1 running, -2 failed, >= 0 number of networks
  if (n == WIFI_SCAN_RUNNING) return true;
  if (n >= 0) scan_report(n); else elog("Wi-Fi scan failed");
  diag = DIAG_DONE;                          // once per boot is enough
  wifi_join();
  return false;
}

void net_init() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NET_HOSTNAME);
  WiFi.setSleep(true);                       // modem sleep is required for Wi-Fi + BLE coexistence
  WiFi.onEvent(wifi_event);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);          // look at every AP with this SSID...
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);      // ...and join the strongest
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  wifi_join();
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  configTzTime(NET_TZ, "pool.ntp.org", "time.google.com");

  // Fingerprint of the compiled-in password: lets you verify the exact bytes without printing them.
  uint8_t h[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)WIFI_PASS, strlen(WIFI_PASS), h);
  snprintf(pwInfo, sizeof(pwInfo), "password %u bytes fp %02x%02x%02x%02x", (unsigned)strlen(WIFI_PASS), h[0], h[1], h[2], h[3]);
  elog("Wi-Fi connecting to %s (%s)", WIFI_SSID, pwInfo);
}

void net_loop() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;

  if (up && !st.wifiOk) {
    snprintf(st.ip, sizeof(st.ip), "%s", WiFi.localIP().toString().c_str());
    elog("Wi-Fi up, IP %s, RSSI %d", st.ip, (int)WiFi.RSSI());
    if (!mdnsStarted) mdnsStarted = MDNS.begin(NET_HOSTNAME);     // the device answers to <NET_HOSTNAME>.local
  }
  if (!up && st.wifiOk) { elog("Wi-Fi lost"); lastDownMs = downSinceMs = now; }
  st.wifiOk = up;
  if (!up) {
    if (diag_scan(now)) return;                                  // the background scan has the radio
    if (now - lastDownMs > 30000) { lastDownMs = now; WiFi.reconnect(); }
    return;
  }
  st.wifiRssi = WiFi.RSSI();

  // NTP -> RTC: once shortly after connect, then hourly. The RTC carries time when Wi-Fi is gone.
  if (now - lastNtpMs > (ntpSynced ? 3600000UL : 5000UL)) {
    lastNtpMs = now;
    struct tm tm;
    if (getLocalTime(&tm, 0) && tm.tm_year + 1900 >= 2025) {
      clock_set_datetime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
      if (!ntpSynced) elog("NTP synced %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
      ntpSynced = true;
    }
  }
}
