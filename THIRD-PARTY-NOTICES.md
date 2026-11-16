# Third-Party Notices

The project's own code is released under The Unlicense (see `LICENSE`). The parts below come from other authors and keep their own licenses.

## Bundled in this repository

| Path | What | Author | License |
|------|------|--------|---------|
| `Thermostat/lib/GFX_Library_for_Arduino/` | Arduino_GFX 1.5.5, display driver | moononournation, based on Adafruit GFX | BSD, see `license.txt` in that folder |
| `Thermostat/lib/SensorLib/` | SensorLib 0.3.1, touch and clock drivers | Lewis He | MIT, see `LICENSE` in that folder |
| `Thermostat/lib/XPowersLib/` | XPowersLib 0.2.9, power chip driver | Lewis He | MIT, see `LICENSE` in that folder |
| `Thermostat/lib/TCA9554/` | TCA9554 0.1.2, I/O expander driver | Rob Tillaart | MIT, see `LICENSE` in that folder |
| `Thermostat/lib/es8311/` | ES8311 audio codec driver | Espressif Systems, adapted to the Arduino Wire library in the Waveshare board examples | Apache-2.0, see `LICENSE` in that folder |
| `Thermostat/src/lv_font_montserrat_96_digits.c`, `Thermostat/src/lv_font_montserrat_64_digits.c` | Digits of the Montserrat Medium typeface, converted to LVGL bitmap fonts | The Montserrat Project Authors | SIL Open Font License 1.1 |
| `Thermostat/include/lv_conf.h` | LVGL configuration, based on LVGL's template | LVGL Kft | MIT |

These libraries are bundled at the exact releases Waveshare ships for the ESP32-S3-Touch-LCD-3.5, because the display was brought up and tested with them.

## Downloaded by PlatformIO at build time

| Library | Author | License |
|---------|--------|---------|
| LVGL 8.4.0 | LVGL Kft | MIT |
| NimBLE-Arduino 2.5.2 | h2zero | Apache-2.0 |
| PubSubClient 2.8.0 | Nicholas O'Leary | MIT |
| OpenTherm Library | Ihor Melnyk | MIT |
| Arduino core for ESP32 and ESP-IDF | Espressif Systems | LGPL-2.1 and Apache-2.0 |

## Hardware

The node uses the Master OpenTherm Shield by DIYLESS Electronics (https://github.com/diyless, https://diyless.com). It is hardware you buy; no DIYLESS code is included here.
