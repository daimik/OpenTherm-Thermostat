#include <cstdio>
#include "node_safety.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
  SafetyParams p; SafetyState s; SafetyOut o;
  // boot, no control yet: fallback heats at 45
  o = ns_apply(s, { 1000, false, 0, false, 0, true, 300 }, p);
  CHECK(o.ch && o.fallback && o.flow_c10 == 450 && !o.hold);
  // control arrives asking for off within min-on time: held on
  o = ns_apply(s, { 60000, true, 100, false, 300, true, 300 }, p);
  CHECK(o.ch && o.hold && !o.fallback);
  // after min-on: off
  o = ns_apply(s, { 1000 + 180000, true, 100, false, 300, true, 300 }, p);
  CHECK(!o.ch && !o.hold);
  uint32_t tOff = 1000 + 180000;
  // asks on again too soon: held off
  o = ns_apply(s, { tOff + 60000, true, 100, true, 550, true, 300 }, p);
  CHECK(!o.ch && o.hold);
  // after min-off: on with requested flow
  o = ns_apply(s, { tOff + 180000, true, 100, true, 550, true, 300 }, p);
  CHECK(o.ch && o.flow_c10 == 550 && !o.clamped);
  // absurd request is clamped
  o = ns_apply(s, { tOff + 181000, true, 100, true, 950, true, 300 }, p);
  CHECK(o.ch && o.flow_c10 == 750 && o.clamped);
  o = ns_apply(s, { tOff + 182000, true, 100, true, 50, true, 300 }, p);
  CHECK(o.ch && o.flow_c10 == 200 && o.clamped);
  // control goes stale: fallback
  o = ns_apply(s, { tOff + 900000, true, 700000, true, 600, true, 300 }, p);
  CHECK(o.fallback && o.ch && o.flow_c10 == 450);

  // frost guard: off requested, return water cold -> heats immediately even inside min-off
  SafetyState f; 
  o = ns_apply(f, { 0, true, 0, false, 300, true, 300 }, p);        CHECK(!o.ch);
  o = ns_apply(f, { 1000, true, 0, true, 400, true, 300 }, p);      CHECK(o.ch);       // first switch
  o = ns_apply(f, { 200000, true, 0, false, 300, true, 300 }, p);   CHECK(!o.ch);      // off after min-on
  o = ns_apply(f, { 210000, true, 0, false, 300, true, 70 }, p);    CHECK(o.ch && o.frost && o.flow_c10 == 350);
  o = ns_apply(f, { 220000, true, 0, false, 300, true, 120 }, p);   CHECK(o.ch && o.frost);                 // inside band
  o = ns_apply(f, { 230000, true, 0, false, 300, true, 160 }, p);   CHECK(o.ch && o.hold && !o.frost);      // guard ended, min-on holds
  o = ns_apply(f, { 210000 + 180000, true, 0, false, 300, true, 160 }, p); CHECK(!o.ch);
  // frost guard ignores return temperature when there is no OpenTherm link
  SafetyState g;
  o = ns_apply(g, { 0, true, 0, false, 300, false, 0 }, p);         CHECK(!o.ch && !o.frost);
  // frost raises a too-low requested flow
  SafetyState h;
  o = ns_apply(h, { 0, true, 0, true, 250, true, 50 }, p);          CHECK(o.ch && o.flow_c10 == 350 && o.frost);

  printf(fails ? "%d FAILED\n" : "all node safety tests passed\n", fails);
  return fails ? 1 : 0;
}
