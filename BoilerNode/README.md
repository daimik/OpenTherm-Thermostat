# 🔥 Boiler Node: Seeed XIAO ESP32C6

Bluetooth peripheral "CV-Boiler" for the thermostat display, OpenTherm master for the boiler, and
the last line of defence: whatever the display asks passes through the safety layer in
`include/node_safety.h` before it reaches the boiler.

The full description is in the [main README](../README.md). This page is the short reference for this project.

---

## 🚀 Builds

Flashing is over USB only.

| Environment | What it is | Command |
|---|---|---|
| `xiao_c6_real` | Real boiler over the OpenTherm adapter | `pio run -e xiao_c6_real -t upload --upload-port COMx` |
| `xiao_c6_real_swapped` | Same, with the two OpenTherm pins exchanged in software | `pio run -e xiao_c6_real_swapped -t upload --upload-port COMx` |
| `xiao_c6` (default) | Simulated boiler, for the desk | `pio run -t upload --upload-port COMx` |

---

## 🔌 Wiring

| OpenTherm adapter | XIAO ESP32-C6 |
|---|---|
| IN | D1 (GPIO1), the node reads the boiler here |
| OUT | D2 (GPIO2), the node sends here |
| VCC | 3V3, never 5V |
| GND | GND |
| two bus terminals | boiler R-Bus / OT terminals, either way round |

---

## 🔒 Files That Are Yours

| File | Content |
|---|---|
| `include/ble_key.h` | Shared Bluetooth key, written by `gen_ble_key.py`. Must be the same as the display's |
| `include/node_secrets.h` | Wi-Fi and page login. Optional: without it the node has no Wi-Fi |
| `include/node_safety.h` | Limits of the safety layer. For floor heating lower `flowMax_c10` |

---

## 🌐 Status Page (optional)

A read-only web page with live values, a one-hour chart and an event log.

1. Copy `include/node_secrets.example.h` to `include/node_secrets.h`.
2. Fill in the Wi-Fi name and password (the two lines can be copied from the display's
   `secrets.h`) and choose a page password that is used nowhere else.
3. Flash. The serial monitor prints `status page at http://<address>/` once Wi-Fi is up.

Deleting `node_secrets.h` and flashing again takes the node off the network.

What it shows:

- Boiler: flow, return, difference, hot water, pressure, modulation, burner starts and burner time
  since start, fault and diagnostic codes.
- Control: what the display asks, the age of the last control frame, what the node sends to the
  boiler, and what the safety layer is doing (fallback, frost guard, limited, hold).
- OpenTherm: outcome of the last exchange, counts of good, timed-out and invalid status exchanges,
  success rate, failed readings, values the boiler does not provide, pins in use.
- Bluetooth: state, paired display, sessions, disconnects with the last reason, rejected frames.
- Node: build, uptime, last reset reason, free memory, Wi-Fi access point and signal.
- Node limits: the fixed limits of the safety layer.
- Chart: flow, return, setpoint sent, modulation and flame for the last hour, one sample per 10 s.
- Event log: the last 200 events. A line is written when something changes (flame, heating enable,
  hot water demand, boiler link, faults, fallback, frost guard, Bluetooth sessions, control changes
  of 2 degrees or more), not on a timer.

| Address | Content |
|---|---|
| `/` | the page |
| `/api/status` | everything on the page as JSON |
| `/api/log?since=N` | event log as JSON, only lines after sequence number N |
| `/api/history` | chart samples as JSON, temperatures in 0.1 degrees |
| `/log.txt` | event log as plain text with date and time |

All are GET. Nothing else is answered, and no request changes anything on the node.

Wi-Fi and the web server run in their own task and start after the display has opened its session,
or after 20 s. The loop that talks to the boiler and the display never waits for them.

To change the page, edit `web/page.html` and run `python web/make_page.py` from this folder. It
writes `include/web_page.h`, the copy the firmware serves.

---

## 🔧 Reading the Serial Monitor

One line every few seconds starts with `st:`. `flags=0x40` with `adv` at the end means no display is
connected and the node is in fallback. `session` at the end means the display is connected and
authenticated. `ot=ok` means the last exchange with the boiler succeeded; `ot=timeout` means the
boiler did not answer.
