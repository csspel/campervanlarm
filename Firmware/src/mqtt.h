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
// Ska anropas regelbundet när MQTT är anslutet.
void mqttLoop();

// Kopplar ner MQTT-klienten.
void mqttDisconnect();

// Returnerar true om MQTT är anslutet just nu.
bool mqttIsConnected();

// Publicerar ett ALIVE-meddelande med aktuell status.
bool mqttPublishAlive();

// Publicerar en enkel GPS-payload ("single").
// fixOk anger om giltig position finns eller inte.
bool mqttPublishGpsSingle(const ExtGnssFix &fx, bool fixOk);

// Publicerar ett PIR-event.
bool mqttPublishPirEvent(uint32_t eventId,
                         uint16_t count,
                         uint32_t firstMs,
                         uint32_t lastMs,
                         uint8_t srcMask);

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