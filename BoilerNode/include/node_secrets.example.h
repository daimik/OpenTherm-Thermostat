// Copy this file to node_secrets.h and fill it in. node_secrets.h stays on your PC: do not share it.
//
// With node_secrets.h present the node joins your Wi-Fi and serves a read-only status page.
// Without it the node is Bluetooth only: no Wi-Fi code runs.
#pragma once

// Same network as the display. The two lines can be copied as they are from Thermostat\include\secrets.h:
// the names WIFI_SSID / WIFI_PASS are accepted here too. In a C string a backslash is written \\ and a
// double quote \".
#define NODE_WIFI_SSID  "your-network"
#define NODE_WIFI_PASS  "your-wifi-password"

#define NODE_HOSTNAME   "cv-boiler"

// Login for the page (HTTP Basic). It travels unencrypted on your network, so use a password that is
// used nowhere else. An empty password "" switches the login off: anyone who can reach the node's
// address can then read the page.
#define NODE_WEB_USER   "admin"
#define NODE_WEB_PASS   "choose-a-password"

// Time zone for the timestamps in /log.txt (the page itself uses the browser's time zone).
#define NODE_TZ         "CET-1CEST,M3.5.0,M10.5.0/3"
