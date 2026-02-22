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
// (byt namn så vi inte krockar med Arduino-kärnan)
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7

// ---------------- SD_MMC (1-bit) ----------------------------
static const int PIN_SD_CLK = 38;
static const int PIN_SD_CMD = 39;
static const int PIN_SD_D0 = 40;

// ---------------- PIR (campervan) ---------------------------
// Du har tidigare använt GPIO9 & GPIO17 för PIR på ESP32-S3.
static const int PIN_PIR_FRONT = 9;
static const int PIN_PIR_BACK = 17;

// De flesta PIR ger HIGH-puls → RISING.
static constexpr bool PIR_RISING_EDGE = true;

// ---------------- Nätinställningar --------------------------
static const char APN[] = "services.telenor.se";

// SIM PIN är känsligt. Lägg det i secrets.h om du använder det.
// Sätt tom sträng om du inte har PIN på SIM:en.
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
static const char MQTT_TOPIC_ALIVE[] = "van/ellie/tele/alive";
static const char MQTT_TOPIC_GPS_SINGLE[] = "van/ellie/tele/gps";
static const char MQTT_TOPIC_PIR[] = "van/ellie/tele/pir";
static const char MQTT_TOPIC_DOWNLINK[] = "van/ellie/cmd/downlink";

// Downlink: PIR ACK (bör vara retain=false från server-sidan)
static const char MQTT_TOPIC_CMD_ACK[] = "van/ellie/cmd/pir_ack";

static const char MQTT_TOPIC_ACK[] = "van/ellie/ack";
static const char MQTT_TOPIC_VERSION[] = "van/ellie/tele/version";

constexpr uint32_t MQTT_ONLINE_WINDOW_MS = 30000UL; // 30 s

static const char DEVICE_ID[] = "van_ellie";

// ============================================================
// Timers
// ============================================================
constexpr uint32_t ALIVE_INTERVAL_MS = 120000UL; // 2 min

// ============================================================
// GPS: start-mode heuristik (TTFF-optimering)
// ============================================================
constexpr uint32_t GPS_HOT_MAX_AGE_MS = 2UL * 60UL * 60UL * 1000UL;   // 2 h
constexpr uint32_t GPS_WARM_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL; // 24 h

// ============================================================
// GPS filter / quality gates (anti "62,15"-spökposition)
// ============================================================

// Rejda helt skräp-DOP (500.0 etc)
constexpr float GPS_HDOP_REJECT_GE = 50.0f;

// I dina loggar dyker 0.1 upp tillsammans med 62/15 och andra orimligheter.
// Så vi kräver ett rimligt intervall när fix_status saknas.
constexpr float GPS_HDOP_MIN = 0.5f;
constexpr float GPS_HDOP_MAX = 10.0f;

// Min satelliter "used" (fältet du läser ur CGNSINF)
constexpr int GPS_SATS_MIN = 4;

// Sanity på höjd och fart (för att stoppa glapp/misparse)
constexpr double GPS_ALT_MIN_M = -200.0;
constexpr double GPS_ALT_MAX_M = 3000.0;
constexpr float GPS_SPEED_MAX_KMH = 200.0f;

// Stabilitet: kräv N bra prover i rad nära varandra innan vi säger valid=true
constexpr uint8_t GPS_STABLE_SAMPLES = 2;
constexpr float GPS_STABLE_DIST_M_STOPPED = 80.0f;
constexpr float GPS_STABLE_DIST_M_MOVING = 250.0f;

// Placeholder som modulen spottar när den inte har fix
constexpr double GPS_PLACEHOLDER_LAT = 62.0;
constexpr double GPS_PLACEHOLDER_LON = 15.0;
constexpr double GPS_PLACEHOLDER_LAT_TOL = 0.05;
constexpr double GPS_PLACEHOLDER_LON_TOL = 0.05;
