#pragma once

#include <Arduino.h>
#include <stdint.h>

struct ExtGnssFix
{
    double lat = 0.0;
    double lon = 0.0;
    float speedKmh = 0.0f;
    float hdop = 99.0f;
    float altM = 0.0f;
    uint8_t fixQuality = 0;
    uint8_t fixMode = 0;
    int sats = 0;
    bool valid = false;
};

bool extGnssBegin(int rxPin, int txPin, uint32_t baud);
void extGnssEnd();
void extGnssPoll();
bool extGnssGetLatest(ExtGnssFix &out);
void extGnssClearLatest();