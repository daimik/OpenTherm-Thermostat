// Read-only status page and event log of the boiler node, over Wi-Fi.
//
// Built in only when include/node_secrets.h exists (copy node_secrets.example.h). Without that file the
// node is exactly what it was: Bluetooth only, no Wi-Fi code running.
//
// The page shows; it does not control. There is no request that changes the heating, the settings or
// the firmware. Everything network-related runs in its own task: if Wi-Fi is down, slow or misbehaving,
// the main loop that talks to the boiler does not wait for it.
#pragma once
#include <stdint.h>
#include "ble_protocol.h"
#include "node_safety.h"

#if __has_include("node_secrets.h")
#define NODE_WEB 1
#else
#define NODE_WEB 0
#endif

// Everything the page shows, collected by the main loop once a second.
struct NodeView {
  BoilerStatus  st;              // boiler values and flags as sent to the display
  // what the display asked for
  bool          haveControl;
  uint32_t      controlAgeMs;
  BoilerControl req;
  uint32_t      ctrlFrames;
  // what the safety layer made of it
  bool          chOn, fallback, frost, clamped, hold;
  // Bluetooth
  uint8_t       bleState;        // 0 advertising, 1 connected but not authenticated, 2 session
  bool          havePeer;
  uint8_t       peer[6];         // paired display, least significant byte first
  uint32_t      sessions, drops, rejected, unauthStreak;
  int           lastDropReason;
  // OpenTherm
  bool          simulated;
  char          otLast[10];      // outcome of the latest status exchange
  uint32_t      otOk, otTimeout, otInvalid, otReadErr, otEdges;
  uint8_t       otRxLevel, pinRx, pinTx;
  uint8_t       otMissing;       // bit 0 hot water temperature, bit 1 pressure, bit 2 outside: the boiler does not provide it
  // burner
  uint32_t      flameStarts, flameOnS;
};

#if NODE_WEB
void web_begin(const char *build, int resetReason, const SafetyParams *limits);
void web_publish(const NodeView &v);
#else
static inline void web_begin(const char *, int, const SafetyParams *) {}
static inline void web_publish(const NodeView &) {}
#endif
