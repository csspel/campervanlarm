#pragma once

// ===== WIFI =====
static const char WIFI_SSID[] = "";
static const char WIFI_PASSWORD[] = "";

// Reserv-WiFi om du vill ha hotspot från mobilen senare
static const char *WIFI_SSID_2 = "";
static const char *WIFI_PASSWORD_2 = "";

// ===== MQTT =====
static const char MQTT_BROKER_HOST[] = "";
static const uint16_t MQTT_BROKER_PORT = 1883;

static const char MQTT_CLIENT_ID[] = "";
static const char MQTT_USERNAME[] = "";
static const char MQTT_PASSWORD[] = "";

// ===== Victron devices =====
// MAC ska helst skrivas med små bokstäver här för enkel jämförelse.
// Orion XS
static const char VICTRON_MAC_1[] = "e3:72:ab:6d:90:00";
static const char VICTRON_KEY_1[] = "12f23f6abef542aa47b6338f09123456";
// SmartSolar
static const char VICTRON_MAC_2[] = "ea:9f:71:34:7c:00";
static const char VICTRON_KEY_2[] = "1201d27ee7c4957982af011c24123456";
// SmartShunt
static const char VICTRON_MAC_3[] = "ea:ed:ca:f4:04:00";
static const char VICTRON_KEY_3[] = "12b93381f94db042c935cd6a2a123456";

// ===== Victron namn =====
static const char VICTRON_NAME_1[] = "orion_xs";
static const char VICTRON_NAME_2[] = "smartsolar";
static const char VICTRON_NAME_3[] = "smartshunt";