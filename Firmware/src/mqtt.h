#pragma once
#include <Arduino.h>
#include "gps.h"

void mqttSetup();
bool mqttConnect();
void mqttLoop();
void mqttDisconnect();
bool mqttIsConnected();
void mqttLoopFor(uint32_t durationMs);

bool mqttPublishVersion(bool retain = true);
bool mqttPublishAlive();

// NY: GPS single
bool mqttPublishGpsSingle(const GpsFix &fx, bool fixOk);

bool mqttPublishPirEvent(uint32_t eventId, uint16_t count, uint32_t firstMs, uint32_t lastMs, uint8_t srcMask);
