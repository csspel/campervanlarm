// logging.cpp - RAM + Serial logging
//
// Goal:
// - No SD_MMC usage at all (it caused instability / errors)
// - Keep existing logging API so the rest of the project compiles
// - Provide a small in-RAM ring buffer for recent lines (optional)

#include "logging.h"
#include "time_manager.h"
#include "config.h"

#include <stdarg.h>
#include <Arduino.h>

#ifndef LOG_RING_LINES
#define LOG_RING_LINES 200
#endif

static String s_ring[LOG_RING_LINES];
static uint16_t s_ringHead = 0;
static uint16_t s_ringCount = 0;

static String makePrefix()
{
  struct tm tm;
  if (getLocalTime(&tm, 0))
  {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return String(buf) + " | " + String(millis() / 1000) + "s | ";
  }

  // fallback om tid inte är synkad än
  return String("---- -- -- --:--:-- | ") + String(millis() / 1000) + "s | ";
}

static void ringPush(const String &line)
{
  s_ring[s_ringHead] = line;
  s_ringHead = (uint16_t)((s_ringHead + 1) % LOG_RING_LINES);
  if (s_ringCount < LOG_RING_LINES)
    s_ringCount++;
}

void loggingInit()
{
  // Nothing to init besides a banner.
  Serial.println("LOG: RAM-only logging");
}

void logSystem(const String &msg)
{
  String line = makePrefix() + msg;
  Serial.println(line);
  ringPush(line);
}

void logSystemf(const char *fmt, ...)
{
  char buf[160]; // öka/minska vid behov
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  logSystem(String(buf));
}