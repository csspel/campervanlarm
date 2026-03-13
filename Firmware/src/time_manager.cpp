#include "time_manager.h"
#include "logging.h"
#include "modem.h"

#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

// ============================================================
// Tidslogik i projektet
// ------------------------------------------------------------
// 1. Systemtiden i ESP32 hålls i UTC.
// 2. Lokal svensk tid fås genom att sätta rätt TZ-regel.
// 3. Tid kan synkas från:
//    - MODEM  (AT+CCLK?)
//    - NTP    (via internet)
// ============================================================

// Tid före detta värde betraktas som ogiltig/skräptid.
// 2024-01-01 00:00:00 UTC
static constexpr time_t kMinValidEpoch = 1704067200;

// POSIX-regel för Sverige / Europe-Stockholm.
// Hanterar automatiskt vintertid och sommartid.
static constexpr const char *kTzPosix = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// Håller reda på senaste kända tidskälla.
static TimeSource g_source = TimeSource::NONE;

// ------------------------------------------------------------
// Sätter systemtid i UTC.
// Returnerar false om epoch är orimligt låg.
// ------------------------------------------------------------
static bool setSystemTimeUtc(time_t epochUtc)
{
  if (epochUtc < kMinValidEpoch)
  {
    return false;
  }

  timeval tv{};
  tv.tv_sec = epochUtc;
  tv.tv_usec = 0;

  return settimeofday(&tv, nullptr) == 0;
}

// ------------------------------------------------------------
// Kompatibilitetsfunktion för att tolka en tm-struktur som UTC.
// Vanlig mktime() tolkar tm som lokal tid, vilket vi inte vill här.
// Därför sätter vi temporärt TZ=UTC0, kör mktime(), och återställer.
// ------------------------------------------------------------
static time_t timegm_compat(tm *t)
{
  char *oldTz = getenv("TZ");
  String old = oldTz ? String(oldTz) : String();

  setenv("TZ", "UTC0", 1);
  tzset();

  // Säkerställ att vi inte råkar få DST-logik här.
  t->tm_isdst = 0;

  time_t epoch = mktime(t);

  // Återställ tidigare tidszon
  if (oldTz)
  {
    setenv("TZ", old.c_str(), 1);
  }
  else
  {
    unsetenv("TZ");
  }
  tzset();

  return epoch;
}

// ------------------------------------------------------------
// Tolkar modemets CCLK-format:
// "yy/MM/dd,hh:mm:ss±zz"
// där zz är antal kvartstimmar relativt UTC.
//
// Exempel:
// "25/12/13,19:22:50+04"
// +04 betyder +60 minuter mot UTC.
//
// Returnerar UTC epoch i outEpochUtc.
// ------------------------------------------------------------
static bool parseCclkToEpochUtc(const String &cclk, time_t &outEpochUtc)
{
  int yy, MM, dd, hh, mm, ss;
  char sign = 0;
  int tzq = 0;

  int matched = sscanf(
      cclk.c_str(),
      "%d/%d/%d,%d:%d:%d%c%d",
      &yy, &MM, &dd, &hh, &mm, &ss, &sign, &tzq);

  if (matched < 8)
  {
    return false;
  }

  // Enkel rimlighetskontroll
  if (yy < 0 || yy > 99 ||
      MM < 1 || MM > 12 ||
      dd < 1 || dd > 31 ||
      hh < 0 || hh > 23 ||
      mm < 0 || mm > 59 ||
      ss < 0 || ss > 59 ||
      (sign != '+' && sign != '-'))
  {
    return false;
  }

  int year = 2000 + yy;

  // Bygg upp tidstrukturen
  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = MM - 1;
  t.tm_mday = dd;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = 0;

  // Tolka inläst tid som om den vore UTC
  time_t epochAssumingUtc = timegm_compat(&t);
  if (epochAssumingUtc <= 0)
  {
    return false;
  }

  // zz = kvartstimmar => minuter
  int offsetMinutes = tzq * 15;

  if (sign == '-')
  {
    offsetMinutes = -offsetMinutes;
  }

  // CCLK anger lokal tid med offset relativt UTC.
  // UTC = lokal tid - offset
  outEpochUtc = epochAssumingUtc - (offsetMinutes * 60);

  return outEpochUtc >= kMinValidEpoch;
}

// ------------------------------------------------------------
// Initierar tidsmodulen.
// - sätter svensk tidszon
// - sätter SNTP till direkt synk
// - nollställer intern state
// ------------------------------------------------------------
void timeInit()
{
  setenv("TZ", kTzPosix, 1);
  tzset();

  // Direkt uppdatering i stället för "smooth"
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  g_source = TimeSource::NONE;
}

// ------------------------------------------------------------
// Returnerar true om systemtiden verkar rimlig.
// ------------------------------------------------------------
bool timeIsValid()
{
  time_t now = time(nullptr);
  return now >= kMinValidEpoch;
}

// ------------------------------------------------------------
// Returnerar senaste tidskälla.
// ------------------------------------------------------------
TimeSource timeGetSource()
{
  return g_source;
}

// ------------------------------------------------------------
// Synka tid från modemet via AT+CCLK?.
// Kräver att modemGetCclk() fungerar.
// ------------------------------------------------------------
bool timeSyncFromModem(uint32_t timeoutMs)
{
  String cclk;

  if (!modemGetCclk(cclk, timeoutMs))
  {
    logSystem("TIME: modem CCLK read failed");
    return false;
  }

  time_t epochUtc = 0;
  if (!parseCclkToEpochUtc(cclk, epochUtc))
  {
    logSystem("TIME: modem CCLK parse failed: " + cclk);
    return false;
  }

  if (!setSystemTimeUtc(epochUtc))
  {
    logSystem("TIME: settimeofday failed (MODEM)");
    return false;
  }

  g_source = TimeSource::MODEM;

  logSystem("TIME: synced from MODEM, epoch=" + String((uint32_t)epochUtc) + ", CCLK=" + cclk);
  return true;
}

// ------------------------------------------------------------
// Synka tid från NTP via internet.
// Väntar tills systemtiden blivit giltig eller timeout nås.
// ------------------------------------------------------------
bool timeSyncFromNtp(uint32_t timeoutMs)
{
  // Starta NTP med svensk tidszon
  configTzTime(kTzPosix, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  uint32_t start = millis();

  while (millis() - start < timeoutMs)
  {
    if (timeIsValid())
    {
      time_t now = time(nullptr);

      g_source = TimeSource::NTP;

      logSystem("TIME: synced from NTP, epoch=" + String((uint32_t)now));
      return true;
    }

    delay(200);
  }

  logSystem("TIME: NTP sync timeout (" + String(timeoutMs) + " ms)");
  return false;
}

// ------------------------------------------------------------
// Returnerar aktuell UTC-tid som Unix epoch.
// ------------------------------------------------------------
uint32_t timeEpochUtc()
{
  return (uint32_t)time(nullptr);
}

// ------------------------------------------------------------
// Returnerar aktuell tid i UTC som ISO8601-sträng.
// Exempel: 2026-03-06T12:34:56Z
// ------------------------------------------------------------
String timeIsoUtc()
{
  time_t now = time(nullptr);

  if (now < kMinValidEpoch)
  {
    return "1970-01-01T00:00:00Z";
  }

  tm t{};
  gmtime_r(&now, &t);

  char buf[25];
  snprintf(buf, sizeof(buf),
           "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year + 1900,
           t.tm_mon + 1,
           t.tm_mday,
           t.tm_hour,
           t.tm_min,
           t.tm_sec);

  return String(buf);
}

// ------------------------------------------------------------
// Returnerar lokalt datum enligt svensk tidszon.
// Exempel: 2026-03-06
// ------------------------------------------------------------
String timeDateLocal()
{
  time_t now = time(nullptr);

  if (now < kMinValidEpoch)
  {
    return "1970-01-01";
  }

  tm t{};
  localtime_r(&now, &t);

  char buf[11];
  snprintf(buf, sizeof(buf),
           "%04d-%02d-%02d",
           t.tm_year + 1900,
           t.tm_mon + 1,
           t.tm_mday);

  return String(buf);
}

// ------------------------------------------------------------
// Returnerar lokalt klockslag enligt svensk tidszon.
// Exempel: 14:23:45
// ------------------------------------------------------------
String timeClockLocal()
{
  time_t now = time(nullptr);

  if (now < kMinValidEpoch)
  {
    return "00:00:00";
  }

  tm t{};
  localtime_r(&now, &t);

  char buf[9];
  snprintf(buf, sizeof(buf),
           "%02d:%02d:%02d",
           t.tm_hour,
           t.tm_min,
           t.tm_sec);

  return String(buf);
}