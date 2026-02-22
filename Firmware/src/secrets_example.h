#pragma once

// ============================================================
//  EXEMPEL – denna fil checkas in i git
//  Skapa en egen src/secrets.h (ligger i .gitignore)
// ============================================================

static const char MQTT_BROKER_HOST[] = "mqtt.example.local";
static const uint16_t MQTT_BROKER_PORT = 1883;

static const char MQTT_CLIENT_ID[] = "campervanlarm";
static const char MQTT_USERNAME[] = "";
static const char MQTT_PASSWORD[] = "";

// Om du har SIM PIN kan du sätta den här (eller i secrets.h):
// #define SIM_PIN "1234"
