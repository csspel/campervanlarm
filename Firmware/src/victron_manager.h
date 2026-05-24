#pragma once

#include <Arduino.h>
#include "profiles.h"

// ============================================================
// Victron BLE manager
// ------------------------------------------------------------
// Adapter mellan den generella VictronBLE-dekodern och
// campervanlarmets pipeline/MQTT-flöde.
//
// Viktig design:
// - Ingen WiFi eller MQTT här.
// - BLE körs bara som kort, schemalagd scan.
// - MQTT-publicering görs av mqtt.cpp.
// ============================================================

void victronManagerInit();

// Returnerar true när aktuell profil får köra Victron BLE och intervallet är nått.
bool victronManagerDue(uint32_t nowMs, const ProfileConfig &profile);

// Kör en blockande BLE-scan. Ska bara anropas när pipeline har sett till
// att kommunikation/radio är avstängd enligt profilen.
bool victronManagerRunScanOnce(uint32_t nowMs, uint32_t scanSeconds);

// Returnerar true om ny Victron-data eller ny scanstatus bör publiceras.
bool victronManagerPublishPending();
void victronManagerClearPublishPending();

// Bygger payload kompatibel med befintlig HA victron.yaml.
String victronManagerBuildStateJson();
