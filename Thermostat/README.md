# 🖥️ Thermostat Display: Waveshare ESP32-S3-Touch-LCD-3.5

The thermostat itself: touch screen, schedule, control loop, room sensor, and the link to the boiler
node. Wi-Fi, MQTT and the status page are extras; heating does not depend on them.

The full description is in the [main README](../README.md). This page is the short reference for this project.

---

## 🚀 Build and Flash

Over USB only:

    pio run -t upload
    pio device monitor

The serial port is set in `platformio.ini` (`upload_port`, `monitor_port`).

---

## 🔒 Files That Are Yours

| File | Content |
|---|---|
| `include/ble_key.h` | Shared Bluetooth key, written by `gen_ble_key.py`. Must be the same as the node's |
| `include/secrets.h` | Wi-Fi, MQTT broker, page login. Copy from `include/secrets.example.h` |

---

## 🔌 Room Sensor

SHT45 on the 32-pin header: 3V3 pin 32, GND pin 30, SDA pin 28 (GPIO8), SCL pin 26 (GPIO7).
The firmware also finds it on a bus of its own: SDA pin 23 (GPIO48), SCL pin 21 (GPIO47).

---

## 🌐 Status Page (optional)

A read-only web page with live values, two 24-hour charts and an event log, at
`http://thermostat.local/` or `http://<address>/`.

Switch it on with two lines in `include/secrets.h`, then flash:

    #define WEB_USER      "admin"
    #define WEB_PASS      "a-password-used-nowhere-else"

Without `WEB_PASS` no web server is started. `WEB_PASS ""` serves the page without a login.

What it shows:

- Room: temperature, humidity, target, mode, state, sensor health, last raw reading, refused readings.
- Control loop: heat demand, error, flow base from the heating curve, proportional and integral
  part, flow asked, outside temperature and its age, learned warm-up rate.
- Schedule: whether Auto follows it, override, preheat, next change, all eight blocks.
- Boiler as the node reports it: temperatures, pressure, flame, burner time and gas estimate today,
  fault, what the node's safety layer is doing.
- Links: Bluetooth to the node, Wi-Fi access point and signal, Home Assistant.
- Device: uptime, last reset, crash counter, memory, slow screen frames.
- Charts: room and target; boiler flow, return, flow asked, outside, modulation and flame.
  One sample a minute, 24 hours.
- Event log: the last 300 events. A line is written when something changes: mode, target (after it
  has stood for 2 s), heating, preheat, sensor, boiler link, flame, faults, Wi-Fi, MQTT, Bluetooth,
  commands from Home Assistant.

Charts and log live in memory and are lost at a restart.

| Address | Content |
|---|---|
| `/` | the page |
| `/api/status` | everything on the page as JSON |
| `/api/log?since=N` | event log as JSON, only lines after sequence number N |
| `/api/history` | chart samples as JSON, temperatures in 0.1 degrees, -32768 = not known |
| `/log.txt` | event log as plain text with date and time |

All are GET. Nothing else is answered, and no request changes anything on the display. Home
Assistant stays the only remote control.

The server runs in its own task on the other processor core and reads a copy of the state. The main
loop never waits for it. It is not started in safe mode.

To change the page, edit `web/page.html` and run `python web/make_page.py` from this folder. It
writes `src/web_page.h`, the copy the firmware serves.
