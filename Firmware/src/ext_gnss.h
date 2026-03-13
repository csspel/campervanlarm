#pragma once

#include <Arduino.h>

// ============================================================
// Senaste kända fix från extern GNSS
// ============================================================
struct ExtGnssFix
{
    bool valid = false; // true om fixen bedöms giltig

    double lat = 0.0;      // latitud i decimalgrader
    double lon = 0.0;      // longitud i decimalgrader
    float speedKmh = 0.0f; // fart i km/h
    float altM = 0.0f;     // höjd över havet i meter

    int sats = 0;       // antal satelliter
    float hdop = 99.0f; // HDOP (lägre är bättre)

    uint8_t fixQuality = 0; // GGA fix quality: 0=no fix, 1=GPS, 2=DGPS, ...
    uint8_t fixMode = 0;    // GSA mode: 1=no fix, 2=2D, 3=3D

    uint32_t epochUtc = 0; // kvar för framtiden, används ej just nu
};

// Starta extern GNSS på angiven UART.
// rxPin = ESP32-pin som tar emot data från GNSS-TX
// txPin = ESP32-pin som skickar data till GNSS-RX
bool extGnssBegin(int rxPin, int txPin, uint32_t baud = 38400);

// Stäng UART mot extern GNSS.
void extGnssEnd();

// Läs inkommande NMEA-data från UART och uppdatera intern senaste fix.
// Ska anropas ofta, t.ex. från pipelineTick().
void extGnssPoll();

// Hämta senaste kända fix.
// Returnerar true om fixen just nu är giltig.
bool extGnssGetLatest(ExtGnssFix &out);