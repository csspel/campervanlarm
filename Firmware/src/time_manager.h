#pragma once
#include <Arduino.h>

enum class TimeSource : uint8_t { NONE=0, MODEM=1, NTP=2 };

void timeInit();                                  // set TZ + internal state
bool timeIsValid();                               // epoch sanity check
TimeSource timeGetSource();                       // last successful sync source
uint32_t timeLastSyncEpochUtc();                  // when we last synced (epoch utc)

// Sync methods
bool timeSyncFromModem(uint32_t timeoutMs = 1500);           // uses AT+CCLK?
bool timeSyncFromNtp(uint32_t timeoutMs = 8000);             // SNTP over IP

// Convenience getters/formatters (require valid time)
uint32_t timeEpochUtc();                          // now epoch utc (seconds)
String   timeIsoUtc();                            // "YYYY-MM-DDTHH:MM:SSZ"
String   timeDateLocal();                         // "YYYY-MM-DD" (Europe/Stockholm)
String   timeClockLocal();                        // "HH:MM:SS" (Europe/Stockholm)