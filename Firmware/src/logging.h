#pragma once

#include <Arduino.h>

// Initierar loggsystemet.
void loggingInit();

// Loggar ett vanligt textmeddelande.
void logSystem(const String &msg);

// printf-liknande loggfunktion.
// Exempel: logSystemf("CSQ=%d", csq);
void logSystemf(const char *fmt, ...);