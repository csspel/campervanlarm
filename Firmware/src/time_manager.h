#pragma once

#include <Arduino.h>

// Anger varifrån systemtiden senast synkades.
enum class TimeSource : uint8_t
{
    NONE = 0,  // Ingen giltig synk ännu
    MODEM = 1, // Tid hämtad från modemet via AT+CCLK?
    NTP = 2    // Tid hämtad från NTP över nätet
};

// Initierar tidsmodulen:
// - sätter lokal tidszon till Europe/Stockholm
// - nollställer intern status för tidskälla/senaste synk
void timeInit();

// Returnerar true om systemtiden verkar giltig.
// Används för att upptäcka om klockan fortfarande är "skräptid".
bool timeIsValid();

// Returnerar vilken källa som senast lyckades synka tiden.
TimeSource timeGetSource();

// Synka tid från modemet via AT+CCLK?
// timeoutMs anger hur länge vi väntar på svar från modemet.
bool timeSyncFromModem(uint32_t timeoutMs = 1500);

// Synka tid från NTP via nätverket.
// timeoutMs anger hur länge vi väntar på att systemtiden ska bli giltig.
bool timeSyncFromNtp(uint32_t timeoutMs = 8000);

// Returnerar aktuell systemtid som UTC epoch (sekunder sedan 1970-01-01).
uint32_t timeEpochUtc();

// Returnerar aktuell tid i UTC-format:
// "YYYY-MM-DDTHH:MM:SSZ"
String timeIsoUtc();

// Returnerar lokalt datum enligt svensk tidszon:
// "YYYY-MM-DD"
String timeDateLocal();

// Returnerar lokalt klockslag enligt svensk tidszon:
// "HH:MM:SS"
String timeClockLocal();