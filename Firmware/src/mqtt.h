#pragma once

#include <Arduino.h>
#include "ext_gnss.h"

// ============================================================
// MQTT API
// ------------------------------------------------------------
// Denna modul ansvarar för:
// - setup av MQTT-klient ovanpå modemets nätverksklient
// - uppkoppling mot broker
// - mottagning av desired state, downlink och PIR-ACK
// - publicering av alive, GPS, PIR-event och ACK
// ============================================================

// Initierar MQTT-lagret och kopplar det till modemets nätverksklient.
// Ansluter inte till broker ännu.
void mqttSetup();

// Kopplar upp mot MQTT-broker och prenumererar på nödvändiga topics.
// Returnerar true om anslutning + subscriptions lyckades.
bool mqttConnect();

// Kör MQTT-klientens loop.
// Returnerar true om MQTT fortfarande är anslutet efter loop-körning.
// Ska anropas regelbundet när MQTT är anslutet.
bool mqttLoop();

// Kopplar ner MQTT-klienten.
void mqttDisconnect();

// Returnerar true om MQTT är anslutet just nu.
bool mqttIsConnected();

// Välj vilken nätverksklient MQTT ska använda.
// mqttUseSimClient() använder modemets TinyGSM-klient.
// mqttUseWifiClient() använder ESP32 WiFiClient.
void mqttUseSimClient();
void mqttUseWifiClient();

// Uppdaterar nätstatus som publiceras på van/ellie/tele/net.
void mqttSetNetStatus(const char *activeLink,
                      bool wifiOk,
                      bool simOk,
                      bool mqttOk,
                      int wifiRssi,
                      int modemRssi,
                      const char *lastFailReason);

// Returnerar aktiv länk enligt MQTT-lagrets senaste status.
const char *mqttGetActiveLink();

// Publicerar ett ALIVE-meddelande med aktuell status.
bool mqttPublishAlive();

// Publicerar health-meddelande med info om senaste recovery, connect och publish.
bool mqttPublishHealth(uint32_t recoveryCountBoot,
                       const char *lastRecoveryReason,
                       uint32_t netConnectCountBoot,
                       uint32_t mqttConnectCountBoot,
                       uint32_t lastNetConnectMs,
                       bool pendingProfileAck,
                       bool pirPending);

// Publicerar en enkel GPS-payload ("single").
// fixOk anger om giltig position finns eller inte.
bool mqttPublishGpsSingle(const ExtGnssFix &fx, bool fixOk);

// Publicerar ett PIR-event.
bool mqttPublishPirEvent(uint32_t eventId,
                         uint16_t count,
                         uint32_t firstMs,
                         uint32_t lastMs,
                         uint8_t srcMask);

// Publicerar en tyst activity boost-händelse från firmware.
// Används främst för PIR fram i PARKED/ARMED.
bool mqttPublishActivityEvent(const char *source,
                              bool boostActive,
                              uint32_t durationS,
                              uint32_t untilMs,
                              const char *detail);

// Returnerar true om ett profile-change ACK väntar på att publiceras.
bool mqttHasPendingProfileAck();

// Publicerar pending profile-change ACK om sådan finns.
// Returnerar true om inget ACK väntar eller om publiceringen lyckades.
// Returnerar false om ACK väntade men inte kunde publiceras.
bool mqttPublishPendingProfileAck();

// Publicerar nätstatus.
// Första versionen rapporterar önskat nätläge och att aktiv länk fortfarande är SIM.
bool mqttPublishNetStatus();

// Publicerar Victron BLE-state om ny scan/data finns.
// Returnerar true om inget behövde publiceras eller om publiceringen lyckades.
bool mqttPublishVictronStateIfPending();

// Returnerar önskat nätläge som senast mottagits från HA.
// Värdet läses även från NVS vid boot så enheten kan välja WiFi/SIM
// innan den hunnit få retained MQTT-state.
const char *mqttGetDesiredNetMode();

// Sätts när HA byter nätläge på ett sätt som kräver ny uppkoppling
// med annan länk, till exempel SIM -> WIFI_ONLY.
bool mqttNetModeSwitchRequested();
void mqttClearNetModeSwitchRequested();

// Aktiv länk enligt MQTT/nätstatus.
// ------------------------------------------------------------
// Desired profile sync-status
// ------------------------------------------------------------
// Returnerar true om device under denna MQTT-anslutning har sett
// ett meddelande på desired-profile-topic (eller legacy downlink
// med desired_profile).
//
// Detta används av pipeline för att veta om retained profil hunnit
// spelas upp efter connect.
// ------------------------------------------------------------
bool mqttHasSeenDesiredProfileThisConnect();