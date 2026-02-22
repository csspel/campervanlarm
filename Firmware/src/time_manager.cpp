#include "time_manager.h"
#include "logging.h"
#include "modem.h"

#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

// ===== config =====
static constexpr time_t kMinValidEpoch = 1704067200;                    // 2024-01-01 00:00:00 UTC
static constexpr const char *kTzPosix = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Europe/Stockholm

static TimeSource g_source = TimeSource::NONE;
static uint32_t g_lastSyncEpoch = 0;

static bool setSystemTimeUtc(time_t epochUtc)
{
  if (epochUtc < kMinValidEpoch)
    return false;
  timeval tv{};
  tv.tv_sec = epochUtc;
  tv.tv_usec = 0;
  return settimeofday(&tv, nullptr) == 0;
}

static time_t timegm_compat(tm *t)
{
  // mktime() tolkar tm som LOCALTIME. Vi vill tolka den som UTC.
  // Lösning: temporärt sätt TZ till UTC0, kör mktime, återställ TZ.
  char *oldTz = getenv("TZ");
  String old = oldTz ? String(oldTz) : String();

  setenv("TZ", "UTC0", 1);
  tzset();
  t->tm_isdst = 0; // säkerhetsgrej

  time_t epoch = mktime(t);

  if (oldTz)
    setenv("TZ", old.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();

  return epoch;
}

static bool parseCclkToEpochUtc(const String &cclk, time_t &outEpochUtc)
{
  // Expected: "yy/MM/dd,hh:mm:ss±zz"  where zz is quarters of an hour
  // Example:  "25/12/13,19:22:50+04"  (+04 => +60 min)
  int yy, MM, dd, hh, mm, ss;
  char sign = 0;
  int tzq = 0;

  if (sscanf(cclk.c_str(), "%d/%d/%d,%d:%d:%d%c%d", &yy, &MM, &dd, &hh, &mm, &ss, &sign, &tzq) < 7)
  {
    return false;
  }

  if (yy < 0 || yy > 99 || MM < 1 || MM > 12 || dd < 1 || dd > 31 ||
      hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59)
  {
    return false;
  }

  int year = 2000 + yy;

  // Build a tm as if the date/time is UTC, then apply tz offset to get UTC epoch.
  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = MM - 1;
  t.tm_mday = dd;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;

  time_t epochAssumingUtc = timegm_compat(&t);
  if (epochAssumingUtc <= 0)
    return false;

  int offsetMinutes = tzq * 15;
  if (sign == '-')
    offsetMinutes = -offsetMinutes;
  // CCLK time is local time with offset relative to UTC -> UTC = local - offset
  outEpochUtc = epochAssumingUtc - (offsetMinutes * 60);

  return outEpochUtc >= kMinValidEpoch;
}

void timeInit()
{
  // Set local time rules for formatting (localtime_r)
  setenv("TZ", kTzPosix, 1);
  tzset();

  // Make SNTP immediate and non-smoothed (we will validate ourselves)
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

  g_source = TimeSource::NONE;
  g_lastSyncEpoch = 0;
}

bool timeIsValid()
{
  time_t now = time(nullptr);
  return now >= kMinValidEpoch;
}

TimeSource timeGetSource() { return g_source; }
// uint32_t timeLastSyncEpochUtc() { return g_lastSyncEpoch; }

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
    logSystem("TIME: modem CCLK parse/validate failed: " + cclk);
    return false;
  }

  if (!setSystemTimeUtc(epochUtc))
  {
    logSystem("TIME: settimeofday failed (MODEM)");
    return false;
  }

  g_source = TimeSource::MODEM;
  g_lastSyncEpoch = (uint32_t)epochUtc;
  logSystem("TIME: synced from MODEM, epoch=" + String((uint32_t)epochUtc) + ", CCLK=" + cclk);
  return true;
}

bool timeSyncFromNtp(uint32_t timeoutMs)
{
  const bool hadValidBefore = timeIsValid();
  const time_t before = time(nullptr);
  const TimeSource beforeSrc = g_source;

  // Configure SNTP (UTC). We handle local formatting via TZ env.
  configTzTime(kTzPosix, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    if (timeIsValid())
    {
      time_t now = time(nullptr);

      if (hadValidBefore && before >= kMinValidEpoch && beforeSrc == TimeSource::MODEM)
      {
        long skew = (long)now - (long)before;
        if (skew < 0)
          skew = -skew;
        if (skew > 600)
        {
          logSystem("TIME: WARNING large MODEM->NTP skew_s=" + String(skew));
        }
        else
        {
          logSystem("TIME: MODEM->NTP skew_s=" + String(skew));
        }
      }

      g_source = TimeSource::NTP;
      g_lastSyncEpoch = (uint32_t)now;
      logSystem("TIME: synced from NTP, epoch=" + String((uint32_t)now));
      setenv("TZ", kTzPosix, 1);
      tzset();

      return true;
    }
    delay(200);
  }

  logSystem("TIME: NTP sync timeout (" + String(timeoutMs) + " ms)");
  return false;
}

uint32_t timeEpochUtc()
{
  time_t now = time(nullptr);
  return (uint32_t)now;
}

String timeIsoUtc()
{
  time_t now = time(nullptr);
  if (now < kMinValidEpoch)
    return "1970-01-01T00:00:00Z";

  tm t{};
  gmtime_r(&now, &t);

  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String timeDateLocal()
{
  time_t now = time(nullptr);
  if (now < kMinValidEpoch)
    return "1970-01-01";

  tm t{};
  localtime_r(&now, &t);

  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(buf);
}

String timeClockLocal()
{
  time_t now = time(nullptr);
  if (now < kMinValidEpoch)
    return "00:00:00";

  tm t{};
  localtime_r(&now, &t);

  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}
