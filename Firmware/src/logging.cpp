#include "logging.h"

#include <Arduino.h>
#include <stdarg.h>

static String makePrefix()
{
  tm timeInfo{};

  if (getLocalTime(&timeInfo, 0))
  {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
    return String(buf) + " | " + String(millis() / 1000) + "s | ";
  }

  // Fallback innan systemtid har synkats
  return String("---- -- -- --:--:-- | ") + String(millis() / 1000) + "s | ";
}

void loggingInit()
{
  Serial.println("LOG: Serial-only logging");
}

void logSystem(const String &msg)
{
  Serial.println(makePrefix() + msg);
}

void logSystemf(const char *fmt, ...)
{
  char buf[160];

  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  logSystem(String(buf));
}