#pragma once
#include <Arduino.h>

// ============================================================
// LilyGO T-SIM7080G-S3 – PIN + RUNTIME KONFIG
// ============================================================

// ---------------- Modem / UART ------------------------------
#define BOARD_MODEM_PWR_PIN 41
#define BOARD_MODEM_RXD_PIN 4
#define BOARD_MODEM_TXD_PIN 5
#define BOARD_MODEM_RI_PIN 3
#define BOARD_MODEM_DTR_PIN 42

// ---------------- I2C till PMU (AXP2101) --------------------
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7

// ---------------- External GNSS -----------------------------
#define EXTERNAL_GNSS_ENABLED 1
#define GPS_ENABLED (INTERNAL_GNSS_ENABLED || EXTERNAL_GNSS_ENABLED)

static const int PIN_GNSS_RX = 18; // ESP32 RX, GNSS TX hit
static const int PIN_GNSS_TX = 17; // ESP32 TX, GNSS RX hit
static const uint32_t GNSS_BAUD = 38400;

// ---------------- PIR (campervan) ---------------------------
static const int PIN_PIR_FRONT = 9;
static const int PIN_PIR_BACK = 10;

// De flesta PIR ger HIGH-puls → RISING.
static constexpr bool PIR_RISING_EDGE = true;

// ---------------- Nätinställningar --------------------------
static const char APN[] = "services.telenor.se";

#ifndef SIM_PIN
static const char SIM_PIN[] = "";
#endif

static const uint32_t NET_REG_TIMEOUT_MS = 120000UL;
static const uint32_t DATA_ATTACH_TIMEOUT_MS = 60000UL;

// ============================================================
// Secrets (MQTT host/user/pass m.m.) – ligger INTE i git
// ============================================================
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets_example.h"
#endif

// ============================================================
// MQTT Topics
// ============================================================

// -------- Uplink / telemetry --------------------------------
static const char MQTT_TOPIC_ALIVE[] = "van/ellie/tele/alive";
static const char MQTT_TOPIC_GPS_SINGLE[] = "van/ellie/tele/gps";
static const char MQTT_TOPIC_PIR[] = "van/ellie/tele/pir";
static const char MQTT_TOPIC_ACK[] = "van/ellie/ack";
static const char MQTT_TOPIC_VERSION[] = "van/ellie/tele/version";

// -------- Ny retained desired-state topic -------------------
// HA publicerar önskad profil här med retain=true.
static const char MQTT_TOPIC_DESIRED_PROFILE[] = "van/ellie/state/desired_profile";

// -------- Legacy / framtida kommandotopic -------------------
// Behålls för migration och ev. framtida engångskommandon.
// Den ska normalt vara retain=false.
static const char MQTT_TOPIC_DOWNLINK[] = "van/ellie/cmd/downlink";

// -------- Event ACK från HA till device ---------------------
// Används idag för PIR_ACK.
static const char MQTT_TOPIC_CMD_ACK[] = "van/ellie/cmd/ack";

constexpr uint32_t MQTT_ONLINE_WINDOW_MS = 30000UL; // 30 s

static const char DEVICE_ID[] = "ellie";

// ============================================================
// Timers
// ============================================================
constexpr uint32_t ALIVE_INTERVAL_MS = 120000UL; // 2 min

// ============================================================
// Boot/profile-sync
// ------------------------------------------------------------
// Efter MQTT connect väntar vi en kort stund på retained profil
// innan vi gör första publish-cykeln.
// Detta hindrar att device startar i default PARKED, publicerar,
// och går ner i RF OFF innan retained profil hunnit tas emot.
// ============================================================
constexpr uint32_t MQTT_BOOT_PROFILE_SYNC_WINDOW_MS = 4000UL; // 4 s

// ============================================================
// GPS: Beslut om du vill använda den interna GNSS:en i SIM7080
// eller en extern GPS via UART/I2C/SPI
// ============================================================
#define INTERNAL_GNSS_ENABLED 0

// ============================================================
// GPS: start-mode heuristik (TTFF-optimering)
// ============================================================
constexpr uint32_t GPS_HOT_MAX_AGE_MS = 2UL * 60UL * 60UL * 1000UL;   // 2 h
constexpr uint32_t GPS_WARM_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL; // 24 h

// ============================================================
// GPS filter / quality gates (anti "62,15"-spökposition)
// ============================================================
constexpr float GPS_HDOP_REJECT_GE = 50.0f;
constexpr float GPS_HDOP_MIN = 0.5f;
constexpr float GPS_HDOP_MAX = 10.0f;
constexpr int GPS_SATS_MIN = 4;
constexpr double GPS_ALT_MIN_M = -200.0;
constexpr double GPS_ALT_MAX_M = 3000.0;
constexpr float GPS_SPEED_MAX_KMH = 200.0f;
constexpr uint8_t GPS_STABLE_SAMPLES = 2;
constexpr float GPS_STABLE_DIST_M_STOPPED = 80.0f;
constexpr float GPS_STABLE_DIST_M_MOVING = 250.0f;
constexpr double GPS_PLACEHOLDER_LAT = 62.0;
constexpr double GPS_PLACEHOLDER_LON = 15.0;
constexpr double GPS_PLACEHOLDER_LAT_TOL = 0.05;
constexpr double GPS_PLACEHOLDER_LON_TOL = 0.05;
constexpr uint32_t GPS_DEV_MAX_WAIT_MS = 8000UL; // 8 s
constexpr bool GPS_DEV_CAP_WAIT = true;