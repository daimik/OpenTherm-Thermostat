// Copy this file to include/secrets.h and fill in your values. secrets.h stays on your PC only.
#pragma once

#define WIFI_SSID     "your-2.4GHz-ssid"
#define WIFI_PASS     "your-wifi-password"

#define MQTT_HOST     "192.168.1.10"      // broker IP or hostname
#define MQTT_PORT     1883
#define MQTT_USER     "thermostat"
#define MQTT_PASS     "mqtt-password"

#define NET_HOSTNAME  "thermostat"        // the device answers to http://thermostat.local

// Optional: read-only status page at http://thermostat.local/ (or the device's IP address).
// The page is built in only when WEB_PASS is defined. The login is HTTP Basic and travels unencrypted
// on your network, so use a password that is used nowhere else. WEB_PASS "" = no login at all.
#define WEB_USER      "admin"
#define WEB_PASS      "choose-a-page-password"
// POSIX TZ for the Netherlands (CET/CEST with automatic summer time)
#define NET_TZ        "CET-1CEST,M3.5.0,M10.5.0/3"

// Optional: MQTT over TLS. Set MQTT_PORT to 8883, MQTT_TLS to 1 and paste the PEM of the CA that
// signed the broker certificate. Only a broker with a certificate from that CA is accepted.
#define MQTT_TLS      0
// #define MQTT_CA_CERT "-----BEGIN CERTIFICATE-----\n" \
//                      "MIIB...each base64 line ends with a backslash-n...\n" \
//                      "-----END CERTIFICATE-----\n"
