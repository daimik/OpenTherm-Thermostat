// Three-screen thermostat UI for 480x320, LVGL 8.4.
// Home: dial + target + tiles + modes. Boiler: values + chart + status. Settings: links + toggles.

#include "ui.h"
#include "state.h"
#include "audio.h"
#include "ble_link.h"
#include "schedule.h"
#include "control.h"
#include "clock.h"
#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

// ---------- palette ----------
#define C_BG     lv_color_hex(0x0B1020)
#define C_CARD   lv_color_hex(0x161D33)
#define C_CARD2  lv_color_hex(0x1F2740)
#define C_TRACK  lv_color_hex(0x222B4A)
#define C_TEXT   lv_color_hex(0xFFFFFF)
#define C_MUTED  lv_color_hex(0xAAB4D4)
#define C_HEAT   lv_color_hex(0xFF8A3D)
#define C_HEAT2  lv_color_hex(0xFF4D6D)
#define C_WARM   lv_color_hex(0xFFB03D)
#define C_COOL   lv_color_hex(0x3DD6F5)
#define C_COOL2  lv_color_hex(0x5B8DEF)
#define C_OK     lv_color_hex(0x5EE2B2)
#define C_OFF    lv_color_hex(0x7C86A8)
#define C_DARK   lv_color_hex(0x0B1020)

// Text on this display is limited to what LVGL's built-in Montserrat fonts contain: ASCII and the degree sign.
// Other characters (for example a middle dot) are drawn as an empty box.
#define C_ICE    lv_color_hex(0x9FD8FF)
#define C_ICE_TRACK lv_color_hex(0x1B3A5C)

static lv_obj_t *scrHome, *scrBoiler, *scrSettings, *scrSched, *scrClock, *scrSleep;
static lv_obj_t *zTime, *zTemp, *zLine, *zPill, *zPillLbl;      // sleep screen
LV_FONT_DECLARE(lv_font_montserrat_96_digits);                  // digits, minus, point and degree sign only
static lv_obj_t *schTime[SCHED_BLOCKS], *schTemp[SCHED_BLOCKS], *schDay[2];
static bool schWeekend = false;
static lv_obj_t *ckDay, *ckHour, *ckMin;

// Home widgets
static lv_obj_t *hArc, *hRoom, *hStatePill, *hStateLbl, *hTarget, *hHum, *hFlow, *hPress, *hModLbl;
static lv_obj_t *hMode[3], *hOutside, *hBleDot, *hWifiDot, *hClock;
static lv_obj_t *hHalo, *hSnow, *hCap;
static lv_timer_t *haloTimer;
static int haloPhase = 0;
// Boiler widgets
static lv_obj_t *bFlow, *bRet, *bDhw, *bBurner, *bPress, *bMod, *bGas, *bFault, *bReset, *bResetLbl, *bChart, *bLink;
static lv_chart_series_t *serFlow, *serRet, *serRoom;
static bool resetArmed = false;
// Settings widgets
static lv_obj_t *sSensorVal, *sWifiVal, *sHaVal, *sOtVal, *sBright, *sVol;

// ---------- helpers ----------
static lv_obj_t *card(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg = C_CARD, int r = 14) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *label(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static lv_obj_t *label_at(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c, int x, int y) {
  lv_obj_t *l = label(p, txt, f, c);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *button(lv_obj_t *p, int x, int y, int w, int h, lv_event_cb_t cb, void *ud, int r = 12) {
  lv_obj_t *b = lv_btn_create(p);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, C_CARD2, 0);
  lv_obj_set_style_bg_color(b, C_TRACK, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, r, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  // Act on touch-down. Waiting for LV_EVENT_CLICKED means waiting for the finger to lift,
  // which adds 100-150 ms to every tap.
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_PRESSED, ud);
  lv_obj_add_event_cb(b, [](lv_event_t *) { audio_play(SND_TICK); }, LV_EVENT_PRESSED, NULL);
  return b;
}

static const char *const kDays[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

// ---- redraw avoidance ----
// LVGL repaints a widget whenever its text or a style is set, even to the same value. ui_update() runs
// every second and on every touch event, so every setter below first checks whether anything changed.
static void set_txt(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}
static void set_txtf(lv_obj_t *l, const char *fmt, ...) {
  char b[96];
  va_list a;
  va_start(a, fmt);
  lv_vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  set_txt(l, b);
}
static void set_tc(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_text_color(o, LV_PART_MAIN).full != c.full) lv_obj_set_style_text_color(o, c, 0);
}
// Remembers a small "visual state" number per widget; true only when it differs from last time.
static bool vis_changed(lv_obj_t *o, int id) {
  intptr_t want = (intptr_t)id + 1;
  if ((intptr_t)lv_obj_get_user_data(o) == want) return false;
  lv_obj_set_user_data(o, (void *)want);
  return true;
}

static void set_gradient(lv_obj_t *o, lv_color_t a, lv_color_t b, lv_style_selector_t sel = 0) {
  lv_obj_set_style_bg_color(o, a, sel);
  lv_obj_set_style_bg_grad_color(o, b, sel);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_HOR, sel);
}

static void clear_gradient(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel = 0) {
  lv_obj_set_style_bg_color(o, c, sel);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_NONE, sel);
}

static lv_obj_t *screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, C_BG, 0);
  lv_obj_set_style_pad_all(s, 0, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static void refresh(lv_obj_t *scr);

static void goto_screen(lv_obj_t *s) {
  refresh(s);                        // bring the target up to date first, then one repaint
  lv_scr_load(s);
}

// ---------- events ----------
static float dragStartTarget = 21.0f;
static void ev_arc(lv_event_t *e) {
  lv_obj_t *a = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_arc_get_value(a);
  v = ((v + 2) / 5) * 5;                          // snap to 0.5 °C
  float t = v / 10.0f;
  if (code == LV_EVENT_PRESSED) {
    dragStartTarget = st.target;                  // remembered so "Off by slider" does not leave a 5° target behind
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    if (t != st.target) {
      audio_play(t > st.target ? SND_UP : SND_DOWN);
      st.target = t;
      schedule_override();                              // in Auto: hold until the next block
      ui_update();
    }
  } else if (code == LV_EVENT_RELEASED) {
    if (v <= 50) {                                // dragged fully left and let go: switch off
      st.target = dragStartTarget;
      st.overrideActive = false;
      st.mode = MODE_OFF;
      audio_play(SND_CONFIRM);
    } else {
      lv_arc_set_value(a, v);
      st.target = t;
      schedule_override();
    }
    ui_update();
  }
}
static void ev_mode(lv_event_t *e) {
  ThermoMode m = (ThermoMode)(intptr_t)lv_event_get_user_data(e);
  if (m == st.mode) return;
  st.mode = m;
  // Without these two calls the dial would be repainted now (new mode, old heating state) and again
  // up to a second later when the control loop catches up. They also get the command to the boiler sooner.
  schedule_tick();
  control_tick();
  ui_update();
}
static void ev_nav_boiler(lv_event_t *)   { goto_screen(scrBoiler); }
static void ev_nav_settings(lv_event_t *) { goto_screen(scrSettings); }
static void ev_nav_home(lv_event_t *)     { goto_screen(scrHome); }
static void ev_reset(lv_event_t *) {
  if (!resetArmed) { resetArmed = true; }
  else { resetArmed = false; audio_play(SND_CONFIRM); ble_link_send_cmd(CMD_RESET_FAULT); Serial.println("BOILER RESET sent"); }
  ui_update();
}
static void ev_toggle(lv_event_t *e) {
  lv_obj_t *sw = lv_event_get_target(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  bool *field = (bool *)lv_event_get_user_data(e);
  *field = on;
  audio_play(on ? SND_UP : SND_DOWN);
  ui_update();
}
static void ev_vol(lv_event_t *e) {
  st.volume = lv_slider_get_value(lv_event_get_target(e));
  audio_set_volume(st.volume);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) audio_play(SND_CONFIRM);
}
static void ev_forget_node(lv_event_t *) {          // long-press the OpenTherm row
  ble_link_forget_node();
  audio_play(SND_CONFIRM);
  ui_update();
}
static void ev_bright(lv_event_t *e) {
  st.brightness = lv_slider_get_value(lv_event_get_target(e));
  display_wake();
  backlight_set(st.brightness);
}

// ---------- state visuals ----------
static lv_point_t snowPts[9][3];

// Six-armed snowflake from lv_line objects (no icon font needed).
static lv_obj_t *build_snowflake(lv_obj_t *parent, int size, lv_color_t color) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_size(box, size, size);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  const float c = size / 2.0f, R = size / 2.0f - 2.0f;
  int n = 0;
  for (int i = 0; i < 3; i++) {                       // three full arms through the centre
    float a = i * (float)M_PI / 3.0f;
    snowPts[n][0] = { (lv_coord_t)(c - R * cosf(a)), (lv_coord_t)(c - R * sinf(a)) };
    snowPts[n][1] = { (lv_coord_t)(c + R * cosf(a)), (lv_coord_t)(c + R * sinf(a)) };
    lv_obj_t *ln = lv_line_create(box);
    lv_line_set_points(ln, snowPts[n], 2);
    lv_obj_set_style_line_color(ln, color, 0);
    lv_obj_set_style_line_width(ln, 3, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    n++;
  }
  for (int i = 0; i < 6; i++) {                       // a small V near each tip
    float a = i * (float)M_PI / 3.0f;
    float bx = c + 0.58f * R * cosf(a), by = c + 0.58f * R * sinf(a);
    float d = 0.34f * R, sp = 0.9f;                   // branch length, spread (rad)
    snowPts[n][0] = { (lv_coord_t)(bx + d * cosf(a - sp)), (lv_coord_t)(by + d * sinf(a - sp)) };
    snowPts[n][1] = { (lv_coord_t)bx, (lv_coord_t)by };
    snowPts[n][2] = { (lv_coord_t)(bx + d * cosf(a + sp)), (lv_coord_t)(by + d * sinf(a + sp)) };
    lv_obj_t *ln = lv_line_create(box);
    lv_line_set_points(ln, snowPts[n], 3);
    lv_obj_set_style_line_color(ln, color, 0);
    lv_obj_set_style_line_width(ln, 2, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    n++;
  }
  return box;
}

// Slow "breathing" halo behind the dial while the boiler is asked for heat. 5 fps, 2.4 s period.
static lv_opa_t haloShown = 0;
static void halo_cb(lv_timer_t *) {
  lv_opa_t want;
  // Each frame repaints the whole dial. While a finger is on the screen (or was within 1.5 s),
  // or another screen is showing, hold a steady glow so touches get the CPU.
  if (lv_scr_act() != scrHome) return;
  if (lv_disp_get_inactive_time(NULL) < 1500) {
    want = 50;
  } else {
    haloPhase = (haloPhase + 1) % 12;
    int tri = haloPhase < 6 ? haloPhase : 12 - haloPhase;      // 0..6..0
    want = (lv_opa_t)(14 + tri * 12);
  }
  if (want != haloShown) { haloShown = want; lv_obj_set_style_arc_opa(hHalo, want, LV_PART_INDICATOR); }
}

// ---------- Home ----------
static void build_home() {
  scrHome = screen();
  lv_obj_t *s = scrHome;

  // top bar
  hClock = label_at(s, "--:--", &lv_font_montserrat_18, C_TEXT, 16, 14);
  hOutside = label_at(s, "", &lv_font_montserrat_14, C_MUTED, 300, 17);
  hBleDot = label_at(s, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_14, C_OFF, 410, 17);
  hWifiDot = label_at(s, LV_SYMBOL_WIFI, &lv_font_montserrat_14, C_OFF, 440, 17);

  // halo (behind the dial, slightly wider band)
  hHalo = lv_arc_create(s);
  lv_obj_set_size(hHalo, 246, 246);
  lv_obj_set_pos(hHalo, 8, 40);
  lv_arc_set_rotation(hHalo, 135);
  lv_arc_set_bg_angles(hHalo, 0, 270);
  lv_arc_set_range(hHalo, 50, 300);
  lv_obj_remove_style(hHalo, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(hHalo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_opa(hHalo, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(hHalo, 34, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(hHalo, C_HEAT, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(hHalo, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(hHalo, false, LV_PART_INDICATOR);
  haloTimer = lv_timer_create(halo_cb, 200, NULL);
  lv_timer_pause(haloTimer);

  // dial
  hArc = lv_arc_create(s);
  lv_obj_set_size(hArc, 230, 230);
  lv_obj_set_pos(hArc, 16, 48);
  lv_arc_set_rotation(hArc, 135);
  lv_arc_set_bg_angles(hArc, 0, 270);
  lv_arc_set_range(hArc, 50, 300);
  lv_obj_set_style_arc_width(hArc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(hArc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(hArc, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(hArc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(hArc, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(hArc, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(hArc, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(hArc, 6, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(hArc, 0, LV_PART_KNOB);
  lv_obj_set_ext_click_area(hArc, 16);
  lv_obj_add_event_cb(hArc, ev_arc, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(hArc, ev_arc, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(hArc, ev_arc, LV_EVENT_PRESSED, NULL);

  hCap = label(s, "LIVING ROOM", &lv_font_montserrat_12, C_MUTED);
  lv_obj_set_style_text_letter_space(hCap, 2, 0);
  lv_obj_set_width(hCap, 170);
  lv_obj_set_style_text_align(hCap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(hCap, hArc, LV_ALIGN_CENTER, 0, -58);

  hRoom = label(s, "--.-°", &lv_font_montserrat_48, C_TEXT);
  lv_obj_set_width(hRoom, 170);
  lv_obj_set_style_text_align(hRoom, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(hRoom, hArc, LV_ALIGN_CENTER, 0, -10);

  hStatePill = card(s, 0, 0, 96, 26, C_CARD2, 13);
  lv_obj_align_to(hStatePill, hArc, LV_ALIGN_CENTER, 0, 36);
  hStateLbl = label(hStatePill, "Idle", &lv_font_montserrat_12, C_MUTED);
  lv_obj_center(hStateLbl);

  hTarget = label(s, "--.-°", &lv_font_montserrat_14, C_WARM);
  lv_obj_set_width(hTarget, 150);
  lv_obj_set_style_text_align(hTarget, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(hTarget, hArc, LV_ALIGN_CENTER, 0, 66);

  hSnow = build_snowflake(s, 44, C_ICE);
  lv_obj_align_to(hSnow, hArc, LV_ALIGN_CENTER, 0, 48);
  lv_obj_add_flag(hSnow, LV_OBJ_FLAG_HIDDEN);

  label_at(s, "5°", &lv_font_montserrat_12, C_MUTED, 46, 258);
  label_at(s, "30°", &lv_font_montserrat_12, C_MUTED, 196, 258);

  // right column x=256..464 (w=208)
  const int cx = 256, cw = 208;

  // tiles 2x2, fill the column
  const int tw = (cw - 8) / 2, th = 64, ty = 48;
  lv_obj_t *t, *l;
  t = card(s, cx, ty, tw, th);
  label_at(t, "Radiators", &lv_font_montserrat_12, C_MUTED, 12, 8);
  hFlow = label_at(t, "--°", &lv_font_montserrat_28, C_TEXT, 12, 27);
  t = card(s, cx + tw + 8, ty, tw, th);
  label_at(t, "Humidity", &lv_font_montserrat_12, C_MUTED, 12, 8);
  hHum = label_at(t, "--%", &lv_font_montserrat_28, C_TEXT, 12, 27);
  t = card(s, cx, ty + th + 8, tw, th);
  label_at(t, "Pressure, bar", &lv_font_montserrat_12, C_MUTED, 12, 8);
  hPress = label_at(t, "-.-", &lv_font_montserrat_28, C_TEXT, 12, 27);
  t = card(s, cx + tw + 8, ty + th + 8, tw, th);
  label_at(t, "Modulation", &lv_font_montserrat_12, C_MUTED, 12, 8);
  hModLbl = label_at(t, "0%", &lv_font_montserrat_28, C_TEXT, 12, 27);


  // mode segments (full column width)
  const int my = 196, mh = 44;
  lv_obj_t *seg = card(s, cx, my, cw, mh, C_CARD2, 12);
  const char *names[3] = {"Auto", "Manual", "Off"};
  const int sw = (cw - 6) / 3;
  for (int i = 0; i < 3; i++) {
    hMode[i] = button(seg, 3 + i * sw, 3, sw, mh - 6, ev_mode, (void *)(intptr_t)i, 10);
    l = label(hMode[i], names[i], &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(l);
  }

  // navigation row
  const int ny = 250, nh = 44, nw = (cw - 8) / 2;
  lv_obj_t *nb = button(s, cx, ny, nw, nh, ev_nav_boiler, NULL, 12);
  l = label(nb, LV_SYMBOL_CHARGE "  Boiler", &lv_font_montserrat_14, C_MUTED); lv_obj_center(l);
  lv_obj_t *ns = button(s, cx + nw + 8, ny, nw, nh, ev_nav_settings, NULL, 12);
  l = label(ns, LV_SYMBOL_SETTINGS "  Settings", &lv_font_montserrat_14, C_MUTED); lv_obj_center(l);
}

// ---------- Boiler ----------
static lv_obj_t *tile(lv_obj_t *p, int x, int y, int w, const char *cap, lv_obj_t **val, lv_color_t vc) {
  lv_obj_t *t = card(p, x, y, w, 54);
  lv_obj_t *c = label_at(t, cap, &lv_font_montserrat_12, C_MUTED, 10, 7);
  lv_obj_set_style_text_letter_space(c, 1, 0);
  *val = label_at(t, "--", &lv_font_montserrat_20, vc, 10, 25);
  return t;
}

static void build_boiler() {
  scrBoiler = screen();
  lv_obj_t *s = scrBoiler;

  lv_obj_t *back = button(s, 16, 12, 32, 32, ev_nav_home, NULL, 10);
  lv_obj_t *l = label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  label_at(s, "Remeha Tzerra Ace 39C", &lv_font_montserrat_16, C_TEXT, 58, 19);
  bLink = label_at(s, LV_SYMBOL_BLUETOOTH " no link", &lv_font_montserrat_12, C_OFF, 390, 21);

  const int tw = 108, gap = 8, y = 54;
  tile(s, 16, y, tw, "RADIATORS", &bFlow, C_WARM);
  tile(s, 16 + (tw + gap), y, tw, "RETURN", &bRet, C_COOL);
  tile(s, 16 + 2 * (tw + gap), y, tw, "HOT WATER", &bDhw, C_TEXT);
  tile(s, 16 + 3 * (tw + gap), y, tw, "BURNER", &bBurner, C_TEXT);

  // chart card
  lv_obj_t *cc = card(s, 16, 118, 290, 188);
  label_at(cc, "Radiators  Return  Room", &lv_font_montserrat_12, C_MUTED, 12, 8);
  label_at(cc, "last 2 h", &lv_font_montserrat_12, C_MUTED, 222, 8);
  bChart = lv_chart_create(cc);
  lv_obj_set_pos(bChart, 10, 28);
  lv_obj_set_size(bChart, 270, 136);
  lv_chart_set_type(bChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(bChart, 24);
  lv_chart_set_range(bChart, LV_CHART_AXIS_PRIMARY_Y, 10, 80);
  lv_chart_set_div_line_count(bChart, 3, 0);
  lv_obj_set_style_bg_opa(bChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bChart, 0, 0);
  lv_obj_set_style_line_color(bChart, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_size(bChart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(bChart, 3, LV_PART_ITEMS);
  serFlow = lv_chart_add_series(bChart, C_HEAT, LV_CHART_AXIS_PRIMARY_Y);
  serRet  = lv_chart_add_series(bChart, C_COOL, LV_CHART_AXIS_PRIMARY_Y);
  serRoom = lv_chart_add_series(bChart, C_MUTED, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(bChart, serFlow, LV_CHART_POINT_NONE);
  lv_chart_set_all_value(bChart, serRet, LV_CHART_POINT_NONE);
  lv_chart_set_all_value(bChart, serRoom, LV_CHART_POINT_NONE);
  label_at(cc, "-2 h", &lv_font_montserrat_12, C_MUTED, 12, 168);
  label_at(cc, "-1 h", &lv_font_montserrat_12, C_MUTED, 132, 168);
  label_at(cc, "now", &lv_font_montserrat_12, C_MUTED, 254, 168);

  // status list
  lv_obj_t *sc = card(s, 314, 118, 150, 138);
  label_at(sc, "Pressure",   &lv_font_montserrat_12, C_MUTED, 12, 12);
  label_at(sc, "Modulation", &lv_font_montserrat_12, C_MUTED, 12, 42);
  label_at(sc, "Gas est.",   &lv_font_montserrat_12, C_MUTED, 12, 72);
  label_at(sc, "Last fault", &lv_font_montserrat_12, C_MUTED, 12, 102);
  bPress = label(sc, "", &lv_font_montserrat_12, C_OK);   lv_obj_align(bPress, LV_ALIGN_TOP_RIGHT, -12, 12);
  bMod   = label(sc, "", &lv_font_montserrat_12, C_TEXT); lv_obj_align(bMod,   LV_ALIGN_TOP_RIGHT, -12, 42);
  bGas   = label(sc, "", &lv_font_montserrat_12, C_TEXT); lv_obj_align(bGas,   LV_ALIGN_TOP_RIGHT, -12, 72);
  bFault = label(sc, "", &lv_font_montserrat_12, C_WARM); lv_obj_align(bFault, LV_ALIGN_TOP_RIGHT, -12, 102);

  bReset = button(s, 314, 264, 150, 42, NULL, NULL, 12);
  lv_obj_add_event_cb(bReset, ev_reset, LV_EVENT_CLICKED, NULL);   // deliberate: press and release on the button
  bResetLbl = label(bReset, LV_SYMBOL_REFRESH "  Reset fault", &lv_font_montserrat_12, C_TEXT);
  lv_obj_center(bResetLbl);
}

// ---------- Settings ----------
static lv_obj_t *link_row(lv_obj_t *p, int y, const char *sym, lv_color_t sc, const char *title, lv_obj_t **val) {
  lv_obj_t *ic = card(p, 12, y, 32, 32, C_CARD2, 10);
  lv_obj_t *l = label(ic, sym, &lv_font_montserrat_14, sc); lv_obj_center(l);
  label_at(p, title, &lv_font_montserrat_14, C_TEXT, 54, y + 1);
  *val = label_at(p, "", &lv_font_montserrat_12, C_MUTED, 54, y + 18);
  return ic;
}

static lv_obj_t *toggle_row(lv_obj_t *p, int y, const char *title, bool *field) {
  lv_obj_t *row = card(p, 10, y, 170, 36, C_CARD2, 10);
  label_at(row, title, &lv_font_montserrat_12, C_TEXT, 10, 11);
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 36, 20);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_bg_color(sw, C_TRACK, LV_PART_MAIN);
  set_gradient(sw, C_WARM, C_HEAT2, LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (*field) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, ev_toggle, LV_EVENT_VALUE_CHANGED, field);
  return row;
}

static void ev_nav_sched(lv_event_t *);
static void ev_nav_clock(lv_event_t *);

static void build_settings() {
  scrSettings = screen();
  lv_obj_t *s = scrSettings;

  lv_obj_t *back = button(s, 16, 12, 32, 32, ev_nav_home, NULL, 10);
  lv_obj_t *l = label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  label_at(s, "Settings", &lv_font_montserrat_16, C_TEXT, 58, 19);
  lv_obj_t *nb = button(s, 252, 8, 110, 38, ev_nav_sched, NULL, 12);
  l = label(nb, LV_SYMBOL_LIST "  Schedule", &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  nb = button(s, 370, 8, 94, 38, ev_nav_clock, NULL, 12);
  l = label(nb, LV_SYMBOL_BELL "  Clock", &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);

  lv_obj_t *lc = card(s, 16, 54, 258, 252);
  lv_obj_t *cap = label_at(lc, "LINKS", &lv_font_montserrat_12, C_MUTED, 12, 10);
  lv_obj_set_style_text_letter_space(cap, 1, 0);
  link_row(lc, 34,  LV_SYMBOL_DRIVE, C_OK,   "Room sensor",   &sSensorVal);
  link_row(lc, 88,  LV_SYMBOL_WIFI,  C_OK,   "Wi-Fi",         &sWifiVal);
  link_row(lc, 142, LV_SYMBOL_HOME,  C_WARM, "Home Assistant", &sHaVal);
  link_row(lc, 196, LV_SYMBOL_CHARGE, C_HEAT, "OpenTherm",     &sOtVal);
  lv_obj_add_flag(sOtVal, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(sOtVal, 14);
  lv_obj_add_event_cb(sOtVal, ev_forget_node, LV_EVENT_LONG_PRESSED, NULL);

  lv_obj_t *dc = card(s, 282, 54, 182, 252);
  cap = label_at(dc, "DISPLAY", &lv_font_montserrat_12, C_MUTED, 12, 10);
  lv_obj_set_style_text_letter_space(cap, 1, 0);
  toggle_row(dc, 34,  "Sleep screen",      &st.sleepScreen);
  toggle_row(dc, 78,  "Sounds",            &st.sounds);

  label_at(dc, "Volume", &lv_font_montserrat_12, C_MUTED, 12, 134);
  sVol = lv_slider_create(dc);
  lv_obj_set_pos(sVol, 12, 160);
  lv_obj_set_size(sVol, 158, 10);
  lv_slider_set_range(sVol, 0, 100);
  lv_slider_set_value(sVol, st.volume, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sVol, C_TRACK, LV_PART_MAIN);
  set_gradient(sVol, C_WARM, C_HEAT2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sVol, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sVol, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(sVol, ev_vol, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(sVol, ev_vol, LV_EVENT_RELEASED, NULL);

  label_at(dc, "Brightness", &lv_font_montserrat_12, C_MUTED, 12, 194);
  sBright = lv_slider_create(dc);
  lv_obj_set_pos(sBright, 12, 220);
  lv_obj_set_size(sBright, 158, 10);
  lv_slider_set_range(sBright, 10, 100);
  lv_slider_set_value(sBright, st.brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sBright, C_TRACK, LV_PART_MAIN);
  set_gradient(sBright, C_COOL, C_COOL2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sBright, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sBright, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(sBright, ev_bright, LV_EVENT_VALUE_CHANGED, NULL);

}

// ---------- Schedule ----------
static SchedBlock *sch_day() { return schWeekend ? sched.weekend : sched.weekday; }

static void sch_refresh() {
  SchedBlock *d = sch_day();
  for (int i = 0; i < SCHED_BLOCKS; i++) {
    lv_label_set_text_fmt(schTime[i], "%02u:%02u", d[i].startMin / 60, d[i].startMin % 60);
    lv_label_set_text_fmt(schTemp[i], "%.1f°", d[i].temp_c10 / 10.0f);
  }
  for (int i = 0; i < 2; i++) {
    lv_obj_t *lbl = lv_obj_get_child(schDay[i], 0);
    if ((i == 1) == schWeekend) { set_gradient(schDay[i], C_WARM, C_HEAT2); lv_obj_set_style_bg_opa(schDay[i], LV_OPA_COVER, 0); lv_obj_set_style_text_color(lbl, C_DARK, 0); }
    else                        { clear_gradient(schDay[i], C_CARD2);       lv_obj_set_style_bg_opa(schDay[i], LV_OPA_TRANSP, 0); lv_obj_set_style_text_color(lbl, C_MUTED, 0); }
  }
}
static void ev_sch_time(lv_event_t *e) {
  int ud = (int)(intptr_t)lv_event_get_user_data(e);
  int i = ud >> 1, dir = (ud & 1) ? 1 : -1;
  SchedBlock *d = sch_day();
  int v = (int)d[i].startMin + dir * 15;
  int lo = i ? d[i - 1].startMin + 15 : 0;
  int hi = i < SCHED_BLOCKS - 1 ? d[i + 1].startMin - 15 : 23 * 60 + 45;
  d[i].startMin = (uint16_t)constrain(v, lo, hi);
  sch_refresh();
}
static void ev_sch_temp(lv_event_t *e) {
  int ud = (int)(intptr_t)lv_event_get_user_data(e);
  int i = ud >> 1, dir = (ud & 1) ? 1 : -1;
  SchedBlock *d = sch_day();
  d[i].temp_c10 = (int16_t)constrain(d[i].temp_c10 + dir * 5, 50, 300);
  sch_refresh();
}
static void ev_sch_day(lv_event_t *e) { schWeekend = (bool)(intptr_t)lv_event_get_user_data(e); sch_refresh(); }
static void ev_sch_back(lv_event_t *) { schedule_save(); goto_screen(scrSettings); }
static void ev_nav_sched(lv_event_t *) { sch_refresh(); goto_screen(scrSched); }

static void build_schedule() {
  scrSched = screen();
  lv_obj_t *s = scrSched;
  lv_obj_t *back = button(s, 16, 12, 32, 32, ev_sch_back, NULL, 10);
  lv_obj_t *l = label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  label_at(s, "Auto schedule", &lv_font_montserrat_16, C_TEXT, 58, 19);

  lv_obj_t *seg = card(s, 264, 8, 200, 40, C_CARD2, 12);
  const char *dn[2] = {"Mon-Fri", "Sat-Sun"};
  for (int i = 0; i < 2; i++) {
    schDay[i] = button(seg, 3 + i * 97, 3, 97, 34, ev_sch_day, (void *)(intptr_t)i, 10);
    l = label(schDay[i], dn[i], &lv_font_montserrat_14, C_MUTED); lv_obj_center(l);
  }

  for (int i = 0; i < SCHED_BLOCKS; i++) {
    lv_obj_t *row = card(s, 16, 54 + i * 63, 448, 56);
    label_at(row, kBlockNames[i], &lv_font_montserrat_16, C_TEXT, 14, 18);

    lv_obj_t *b = button(row, 104, 8, 40, 40, ev_sch_time, (void *)(intptr_t)(i * 2), 10);
    l = label(b, LV_SYMBOL_MINUS, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
    schTime[i] = label(row, "00:00", &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_width(schTime[i], 76);
    lv_obj_set_style_text_align(schTime[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(schTime[i], 146, 16);
    b = button(row, 224, 8, 40, 40, ev_sch_time, (void *)(intptr_t)(i * 2 + 1), 10);
    l = label(b, LV_SYMBOL_PLUS, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);

    b = button(row, 280, 8, 40, 40, ev_sch_temp, (void *)(intptr_t)(i * 2), 10);
    l = label(b, LV_SYMBOL_MINUS, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
    schTemp[i] = label(row, "--.-°", &lv_font_montserrat_20, C_WARM);
    lv_obj_set_width(schTemp[i], 76);
    lv_obj_set_style_text_align(schTemp[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(schTemp[i], 322, 16);
    b = button(row, 400, 8, 40, 40, ev_sch_temp, (void *)(intptr_t)(i * 2 + 1), 10);
    l = label(b, LV_SYMBOL_PLUS, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  }
  sch_refresh();
}

// ---------- Clock ----------
static lv_obj_t *roller(lv_obj_t *p, int x, int y, int w, const char *opts) {
  lv_obj_t *r = lv_roller_create(p);
  lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(r, 3);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_width(r, w);
  lv_obj_set_style_bg_color(r, C_CARD, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_radius(r, 14, 0);
  lv_obj_set_style_text_color(r, C_MUTED, 0);
  lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
  lv_obj_set_style_bg_color(r, C_CARD2, LV_PART_SELECTED);
  lv_obj_set_style_text_color(r, C_TEXT, LV_PART_SELECTED);
  lv_obj_set_style_text_font(r, &lv_font_montserrat_20, LV_PART_SELECTED);
  return r;
}
static void ev_ck_save(lv_event_t *) {
  clock_set(lv_roller_get_selected(ckDay), lv_roller_get_selected(ckHour), lv_roller_get_selected(ckMin));
  audio_play(SND_CONFIRM);
  ui_update();
  goto_screen(scrSettings);
}
static void ev_ck_back(lv_event_t *) { goto_screen(scrSettings); }
static void ev_nav_clock(lv_event_t *) {
  lv_roller_set_selected(ckDay, st.clockOk ? st.dow : 0, LV_ANIM_OFF);
  lv_roller_set_selected(ckHour, st.hour % 24, LV_ANIM_OFF);
  lv_roller_set_selected(ckMin, st.minute % 60, LV_ANIM_OFF);
  goto_screen(scrClock);
}

static void build_clock() {
  static char hours[24 * 3 + 1], mins[60 * 3 + 1];
  char *p = hours;
  for (int i = 0; i < 24; i++) p += sprintf(p, i < 23 ? "%02d\n" : "%02d", i);
  p = mins;
  for (int i = 0; i < 60; i++) p += sprintf(p, i < 59 ? "%02d\n" : "%02d", i);

  scrClock = screen();
  lv_obj_t *s = scrClock;
  lv_obj_t *back = button(s, 16, 12, 32, 32, ev_ck_back, NULL, 10);
  lv_obj_t *l = label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, C_TEXT); lv_obj_center(l);
  label_at(s, "Clock", &lv_font_montserrat_16, C_TEXT, 58, 19);

  label_at(s, "DAY", &lv_font_montserrat_12, C_MUTED, 46, 60);
  label_at(s, "HOUR", &lv_font_montserrat_12, C_MUTED, 246, 60);
  label_at(s, "MINUTE", &lv_font_montserrat_12, C_MUTED, 356, 60);
  ckDay  = roller(s, 40, 80, 180, "Monday\nTuesday\nWednesday\nThursday\nFriday\nSaturday\nSunday");
  ckHour = roller(s, 240, 80, 90, hours);
  ckMin  = roller(s, 350, 80, 90, mins);

  lv_obj_t *save = button(s, 140, 252, 200, 48, ev_ck_save, NULL, 14);
  set_gradient(save, C_WARM, C_HEAT2);
  l = label(save, "Save", &lv_font_montserrat_16, C_DARK); lv_obj_center(l);
}

// ---------- Sleep screen ----------
// Shown after a minute without touch: room temperature large enough to read across the room,
// mode and target on one line, a pill only while the boiler is asked for heat. Plain text on black,
// so it costs almost nothing to keep up to date.
static void build_sleep() {
  scrSleep = screen();
  lv_obj_t *s = scrSleep;
  lv_obj_set_style_bg_color(s, lv_color_hex(0x05080F), 0);

  zTime = label_at(s, "", &lv_font_montserrat_14, lv_color_hex(0x6F7A99), 16, 12);

  zTemp = label(s, "--.-°", &lv_font_montserrat_96_digits, lv_color_hex(0xE8EDFA));
  lv_obj_set_width(zTemp, 460);
  lv_obj_set_style_text_align(zTemp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(zTemp, LV_ALIGN_TOP_MID, 0, 86);

  zLine = label(s, "", &lv_font_montserrat_20, lv_color_hex(0xC9D2EA));
  lv_obj_set_width(zLine, 460);
  lv_obj_set_style_text_align(zLine, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(zLine, LV_ALIGN_TOP_MID, 0, 190);

  zPill = card(s, 0, 0, 132, 30, C_CARD2, 15);
  lv_obj_align(zPill, LV_ALIGN_TOP_MID, 0, 240);
  zPillLbl = label(zPill, "Heating", &lv_font_montserrat_14, C_DARK);
  lv_obj_center(zPillLbl);
  lv_obj_add_flag(zPill, LV_OBJ_FLAG_HIDDEN);
}

void ui_sleep_enter() {
  if (lv_scr_act() == scrSleep) return;
  if (lv_scr_act() == scrSched) schedule_save();     // do not lose edits because the screen went to sleep
  resetArmed = false;
  goto_screen(scrSleep);
}

void ui_sleep_exit() {
  if (lv_scr_act() == scrSleep) goto_screen(scrHome);
}

// ---------- public ----------
static void update_home();
static void update_boiler();
static void update_settings();
static void update_sleep();

void ui_init() {
  build_home();
  build_boiler();
  build_settings();
  build_schedule();
  build_clock();
  build_sleep();
  update_home(); update_boiler(); update_settings(); update_sleep();
  lv_scr_load(scrHome);
}

void ui_history_tick() {
  static uint32_t n = 290;                     // first sample ~10 s after boot, then every 5 min
  if (++n < 300) return;
  n = 0;
  bool b = st.bleLinked && st.boilerLinked;
  lv_chart_set_next_value(bChart, serFlow, b ? (lv_coord_t)lroundf(st.flow) : LV_CHART_POINT_NONE);
  lv_chart_set_next_value(bChart, serRet,  b ? (lv_coord_t)lroundf(st.ret)  : LV_CHART_POINT_NONE);
  lv_chart_set_next_value(bChart, serRoom, st.sensorOk ? (lv_coord_t)lroundf(st.room) : LV_CHART_POINT_NONE);
  lv_chart_refresh(bChart);
}

// ---------- refresh ----------
// One function per screen. Only the screen that is showing is refreshed each second; a screen is also
// refreshed right before it is shown. Every setter compares first, so an unchanged value costs no repaint.

static void update_home() {
  bool heating = st.heating;
  bool off = st.mode == MODE_OFF;
  bool dragging = (lv_obj_get_state(hArc) & LV_STATE_PRESSED) != 0;

  set_txtf(hRoom, st.sensorOk ? "%.1f°" : "--.-°", st.room);
  if (dragging && lv_arc_get_value(hArc) <= 52)
    set_txt(hTarget, "release to turn off");
  else if (st.mode == MODE_AUTO && st.autoActive && st.preheat)
    set_txtf(hTarget, LV_SYMBOL_RIGHT " %.1f°  for %02u:%02u", st.target, st.nextChangeMin / 60, st.nextChangeMin % 60);
  else if (st.mode == MODE_AUTO && st.autoActive)
    set_txtf(hTarget, LV_SYMBOL_RIGHT " %.1f°  until %02u:%02u", st.target, st.nextChangeMin / 60, st.nextChangeMin % 60);
  else if (st.mode == MODE_AUTO)
    set_txtf(hTarget, LV_SYMBOL_RIGHT " %.1f°  set clock", st.target);
  else
    set_txtf(hTarget, LV_SYMBOL_RIGHT " %.1f°", st.target);
  set_tc(hTarget, heating ? C_WARM : C_MUTED);
  if (st.clockOk) set_txtf(hClock, "%s %02u:%02u", kDays[st.dow % 7], st.hour, st.minute);
  else            set_txt(hClock, "Set clock");
  if (!dragging) lv_arc_set_value(hArc, (int)lroundf(st.target * 10));
  lv_arc_set_value(hHalo, lv_arc_get_value(hArc));

  // state-driven look: OFF = ice, HEATING = warm with breathing halo, IDLE = calm cyan
  static int8_t lastLook = -1;
  int8_t look = off ? 0 : (heating ? 1 : 2);
  if (look != lastLook) {
    lastLook = look;
    lv_obj_set_style_arc_color(hArc, off ? C_ICE_TRACK : C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(hArc, heating ? C_HEAT : C_COOL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(hArc, off ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(hArc, off ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
    set_tc(hRoom, off ? C_ICE : C_TEXT);
    if (off) { lv_obj_add_flag(hStatePill, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(hTarget, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(hSnow, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(hArc, LV_OBJ_FLAG_CLICKABLE); }
    else     { lv_obj_clear_flag(hStatePill, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(hTarget, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(hSnow, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(hArc, LV_OBJ_FLAG_CLICKABLE); }
    if (heating) { haloPhase = 0; lv_timer_resume(haloTimer); set_gradient(hStatePill, C_WARM, C_HEAT2); set_tc(hStateLbl, C_DARK); }
    else         { lv_timer_pause(haloTimer); haloShown = 0; lv_obj_set_style_arc_opa(hHalo, LV_OPA_TRANSP, LV_PART_INDICATOR);
                   clear_gradient(hStatePill, C_CARD2); set_tc(hStateLbl, C_COOL); }
  }

  // state pill and caption, in words
  static const char *const kWords[4] = {"Idle", "Heating", "Preheating", "No sensor"};
  set_txt(hStateLbl, kWords[!heating ? 0 : (!st.sensorOk ? 3 : (st.preheat ? 2 : 1))]);
  static const char *const kCaps[3] = {"LIVING ROOM", "FROST GUARD", "SENSOR FAULT"};
  int cap = st.sensorFault ? 2 : (st.frostActive ? 1 : 0);
  set_txt(hCap, kCaps[cap]);
  set_tc(hCap, cap == 2 ? C_HEAT2 : ((cap == 1 || off) ? C_ICE : C_MUTED));

  // tiles
  bool boiler = st.bleLinked && st.boilerLinked;
  set_txtf(hFlow, boiler ? "%.0f°" : "--°", st.flow);
  set_txtf(hHum, st.sensorOk ? "%.0f%%" : "--%%", st.humidity);
  set_txtf(hPress, boiler ? "%.1f" : "-.-", st.pressure);
  set_txtf(hModLbl, "%d%%", boiler && st.flame ? st.modulation : 0);

  // top bar
  if (st.safeMode)            { set_txt(hOutside, "SAFE MODE"); set_tc(hOutside, C_HEAT2); }
  else {
    set_tc(hOutside, C_MUTED);
    if (st.outsideOk) set_txtf(hOutside, "Outside %.1f°", st.outside);
    else set_txt(hOutside, "");
  }
  set_tc(hBleDot, st.bleLinked ? (st.boilerLinked ? C_OK : C_WARM) : C_OFF);
  set_tc(hWifiDot, st.wifiOk ? C_OK : C_OFF);

  // mode selector
  for (int i = 0; i < 3; i++) {
    bool sel = i == (int)st.mode;
    if (!vis_changed(hMode[i], sel ? (off ? 2 : 1) : 0)) continue;
    lv_obj_t *lbl = lv_obj_get_child(hMode[i], 0);
    if (sel) { if (off) set_gradient(hMode[i], C_COOL, C_COOL2); else set_gradient(hMode[i], C_WARM, C_HEAT2); lv_obj_set_style_bg_opa(hMode[i], LV_OPA_COVER, 0); set_tc(lbl, C_DARK); }
    else     { clear_gradient(hMode[i], C_CARD2); lv_obj_set_style_bg_opa(hMode[i], LV_OPA_TRANSP, 0); set_tc(lbl, C_MUTED); }
  }
}

static void update_boiler() {
  bool boiler = st.bleLinked && st.boilerLinked;     // without a link the last values mean nothing: show dashes
  set_txtf(bFlow, boiler ? "%.0f°" : "--", st.flow);
  set_txtf(bRet,  boiler ? "%.0f°" : "--", st.ret);
  set_txtf(bDhw,  boiler ? "%.0f°" : "--", st.dhw);
  set_txtf(bBurner, "%luh%02lu", (unsigned long)(st.burnerSecondsToday / 3600), (unsigned long)((st.burnerSecondsToday / 60) % 60));
  set_txtf(bPress, boiler ? "%.1f bar" : "--", st.pressure);
  set_txtf(bMod, boiler ? "%d%%" : "--", boiler && st.flame ? st.modulation : 0);
  set_txtf(bGas, "%.1f m3", st.gasToday);
  set_txt(bFault, st.boilerFault ? st.lastFault : "none");

  if (!st.bleLinked && st.bleVersionMismatch) { set_txt(bLink, LV_SYMBOL_WARNING " mismatch");  set_tc(bLink, C_HEAT2); }
  else if (!st.bleLinked && st.bleAuthFailed) { set_txt(bLink, LV_SYMBOL_WARNING " auth fail"); set_tc(bLink, C_HEAT2); }
  else if (!st.bleLinked)       { set_txt(bLink, LV_SYMBOL_BLUETOOTH " no link");   set_tc(bLink, C_OFF); }
  else if (st.boilerSimulated)  { set_txt(bLink, LV_SYMBOL_BLUETOOTH " simulated"); set_tc(bLink, C_WARM); }
  else if (st.boilerFallback)   { set_txt(bLink, LV_SYMBOL_BLUETOOTH " fallback");  set_tc(bLink, C_WARM); }
  else                          { set_txt(bLink, LV_SYMBOL_BLUETOOTH " linked");    set_tc(bLink, C_OK); }

  set_txt(bResetLbl, resetArmed ? "Tap again to confirm" : LV_SYMBOL_REFRESH "  Reset fault");
  if (vis_changed(bReset, resetArmed ? 1 : 0)) {
    if (resetArmed) { set_gradient(bReset, C_WARM, C_HEAT2); set_tc(bResetLbl, C_DARK); }
    else            { clear_gradient(bReset, C_CARD2);       set_tc(bResetLbl, C_TEXT); }
  }
}

static void update_settings() {
  if (st.sensorOk)             set_txtf(sSensorVal, "SHT45 %.1f° %.0f%%", st.room, st.humidity);
  else if (!st.sensorPresent)  set_txt(sSensorVal, "not found, check the wiring");
  else                         set_txt(sSensorVal, "fault, no plausible reading");

  if (st.wifiOk) set_txtf(sWifiVal, "%s %d dBm", st.ip, st.wifiRssi);
  else           set_txt(sWifiVal, "not connected");
  set_txt(sHaVal, st.haOk ? "MQTT online" : (st.wifiOk ? "MQTT broker unreachable" : "needs Wi-Fi"));

  if (st.bleLinked) {
    const char *what = st.boilerSimulated ? "simulated" : (st.boilerLinked ? "OT ok" : "OT down");
    const char *note = st.nodeFrost ? " frost" : (st.nodeClamped ? " limited" : (st.nodeHold ? " hold" : ""));   // what the node's safety layer is doing
    set_txtf(sOtVal, "node %s %d dBm%s", what, st.bleRssi, note);
  }
  else if (st.bleVersionMismatch) set_txt(sOtVal, "node firmware does not match");
  else if (st.bleAuthFailed)      set_txt(sOtVal, "node failed authentication!");
  else                            set_txt(sOtVal, st.nodePinned ? "looking for paired node" : "looking for a node to pair");

}

static void update_sleep() {
  bool off = st.mode == MODE_OFF;
  if (st.clockOk) set_txtf(zTime, "%s %02u:%02u", kDays[st.dow % 7], st.hour, st.minute);
  else            set_txt(zTime, "");
  if (st.sensorOk) set_txtf(zTemp, "%.1f°", st.room);
  else             set_txt(zTemp, "--.-°");
  set_tc(zTemp, off ? C_ICE : lv_color_hex(0xE8EDFA));
  if (off)                        set_txt(zLine, "Off");
  else if (st.mode == MODE_AUTO)  set_txtf(zLine, "Auto   target %.1f°", st.target);
  else                            set_txtf(zLine, "Manual   target %.1f°", st.target);

  int pill = !st.heating ? 0 : (st.frostActive ? 3 : (st.preheat ? 2 : 1));
  if (vis_changed(zPill, pill)) {
    static const char *const kPill[4] = {"", "Heating", "Preheating", "Frost guard"};
    if (pill == 0) lv_obj_add_flag(zPill, LV_OBJ_FLAG_HIDDEN);
    else {
      lv_obj_clear_flag(zPill, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(zPillLbl, kPill[pill]);
      if (pill == 3) set_gradient(zPill, C_COOL, C_COOL2); else set_gradient(zPill, C_WARM, C_HEAT2);
    }
  }
}

static void refresh(lv_obj_t *scr) {
  if (scr == scrHome)          update_home();
  else if (scr == scrBoiler)   update_boiler();
  else if (scr == scrSettings) update_settings();
  else if (scr == scrSleep)    update_sleep();
  // Schedule and Clock redraw themselves from their own handlers
}

void ui_update() { refresh(lv_scr_act()); }
