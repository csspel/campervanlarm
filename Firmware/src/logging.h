#pragma once
#include <Arduino.h>

void loggingInit();
void logSystem(const String &msg);
void logSystemf(const char *fmt, ...);