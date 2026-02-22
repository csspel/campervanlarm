#include <Arduino.h>
#include <math.h>
#include "gps.h"
#include "logging.h"
#include "config.h"
#include "time_manager.h"

// We reuse the same UART that TinyGSM uses.
// SerialAT is defined in modem.cpp with external linkage.
extern HardwareSerial SerialAT;

static bool g_gpsOn = false;
static bool g_hasFix = false;
static GpsFix g_lastFix;
static uint32_t g_lastFixAtMs = 0;

// Acquisition tracking (för TTFF)
static uint32_t g_acqStartMs = 0;
static bool g_acqActive = false;

// Track what start mode we requested
static const char *g_lastStartCmd = "AT+CGNSCOLD";

// Stabilitetsfilter (N prover i rad)
static bool g_hasCandidate = false;
static GpsFix g_lastCandidate;
static uint8_t g_stableCount = 0;

// ----------------- Minimal AT helper -----------------
static void atFlush()
{
  while (SerialAT.available())
    (void)SerialAT.read();
}

static bool atWaitOk(uint32_t timeoutMs)
{
  uint32_t start = millis();
  String line;
  while (millis() - start < timeoutMs)
  {
    while (SerialAT.available())
    {
      char c = (char)SerialAT.read();
      if (c == '\r')
        continue;
      if (c == '\n')
      {
        line.trim();
        if (line.length())
        {
          if (line == "OK")
            return true;
          if (line == "ERROR")
            return false;
        }
        line = "";
      }
      else
      {
        line += c;
      }
    }
    delay(5);
  }
  return false;
}

static bool atCmdOk(const String &cmd, uint32_t timeoutMs = 2000)
{
  atFlush();
  SerialAT.println(cmd);
  return atWaitOk(timeoutMs);
}

static bool atCmdGetLine(const String &cmd, const String &prefix, String &outLine, uint32_t timeoutMs = 2000)
{
  atFlush();
  SerialAT.println(cmd);

  uint32_t start = millis();
  String line;
  bool got = false;

  while (millis() - start < timeoutMs)
  {
    while (SerialAT.available())
    {
      char c = (char)SerialAT.read();
      if (c == '\r')
        continue;
      if (c == '\n')
      {
        line.trim();
        if (line.length())
        {
          if (line.startsWith(prefix))
          {
            outLine = line;
            got = true;
          }
          if (line == "OK")
            return got;
          if (line == "ERROR")
            return false;
        }
        line = "";
      }
      else
      {
        line += c;
      }
    }
    delay(5);
  }
  return got;
}

// ----------------- Geodesi -----------------
static inline double deg2rad(double d) { return d * (M_PI / 180.0); }

static float haversine_m(double lat1, double lon1, double lat2, double lon2)
{
  const double R = 6371000.0;
  double dLat = deg2rad(lat2 - lat1);
  double dLon = deg2rad(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) + cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return (float)(R * c);
}

// ----------------- start mode helpers -----------------
static uint8_t cmdToStartModeEnum(const char *cmd)
{
  if (!cmd)
    return 0;
  if (strcmp(cmd, "AT+CGNSCOLD") == 0)
    return 1;
  if (strcmp(cmd, "AT+CGNSWARM") == 0)
    return 2;
  if (strcmp(cmd, "AT+CGNSHOT") == 0)
    return 3;
  return 0;
}

static const char *cmdToModeStr(const char *cmd)
{
  if (!cmd)
    return "UNKNOWN";
  if (strcmp(cmd, "AT+CGNSHOT") == 0)
    return "HOT";
  if (strcmp(cmd, "AT+CGNSWARM") == 0)
    return "WARM";
  if (strcmp(cmd, "AT+CGNSCOLD") == 0)
    return "COLD";
  return "UNKNOWN";
}

static const char *pickStartCmd()
{
  // Ingen tid -> cold
  if (!timeIsValid())
    return "AT+CGNSCOLD";

  // Ingen tidigare fix -> kör cold (stabilt)
  if (!g_hasFix)
    return "AT+CGNSCOLD";

  uint32_t age = millis() - g_lastFixAtMs;
  if (age <= GPS_HOT_MAX_AGE_MS)
    return "AT+CGNSHOT";
  if (age <= GPS_WARM_MAX_AGE_MS)
    return "AT+CGNSWARM";
  return "AT+CGNSCOLD";
}

// ----------------- gates -----------------
static bool isPlaceholder(double lat, double lon)
{
  return (fabs(lat - GPS_PLACEHOLDER_LAT) < GPS_PLACEHOLDER_LAT_TOL) &&
         (fabs(lon - GPS_PLACEHOLDER_LON) < GPS_PLACEHOLDER_LON_TOL);
}

static bool coordsRangeOk(double lat, double lon)
{
  return (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0);
}

static bool coordsNearZero(double lat, double lon)
{
  return (fabs(lat) < 0.001) && (fabs(lon) < 0.001);
}

static bool sanityOk(const GpsFix &x)
{
  if (!coordsRangeOk(x.lat, x.lon))
    return false;
  if (coordsNearZero(x.lat, x.lon))
    return false;
  if (isPlaceholder(x.lat, x.lon))
    return false;

  if (x.hdop >= GPS_HDOP_REJECT_GE)
    return false;
  if (x.alt_m < GPS_ALT_MIN_M || x.alt_m > GPS_ALT_MAX_M)
    return false;
  if (x.speed_kmh < 0.0 || x.speed_kmh > GPS_SPEED_MAX_KMH)
    return false;

  return true;
}

static bool qualityGate(const GpsFix &x)
{
  if (x.run_status != 1)
    return false;
  if (x.utc.length() < 8)
    return false;
  if (!sanityOk(x))
    return false;

  if (x.sats_used < GPS_SATS_MIN)
    return false;
  if (x.fix_mode < 1)
    return false;

  // HDOP-window
  if (!(x.hdop >= GPS_HDOP_MIN && x.hdop <= GPS_HDOP_MAX))
    return false;

  // Om fix-fältet finns: kräv fix_status==1
  if (x.fix_field_present)
  {
    return (x.fix_status == 1);
  }

  // Om fix-fältet saknas: vi är extra hårda redan (sats+hdop+mode+sanity)
  return true;
}

static bool stabilityGate(const GpsFix &x)
{
  if (!g_hasCandidate)
    return true;
  float d = haversine_m(g_lastCandidate.lat, g_lastCandidate.lon, x.lat, x.lon);
  float limit = (x.speed_kmh > 1.0f) ? GPS_STABLE_DIST_M_MOVING : GPS_STABLE_DIST_M_STOPPED;
  return (d <= limit);
}

// ----------------- CGNSINF parse -----------------
// +CGNSINF: <run>,<fix>,<utc>,<lat>,<lon>,<alt>,<spd>,<cog>,<fix_mode>,...
static bool parseCgnsinf(const String &line, GpsFix &out)
{
  int colon = line.indexOf(':');
  if (colon < 0)
    return false;

  String csv = line.substring(colon + 1);
  csv.trim();

  // split (KEEP empty fields!)
  const int MAXF = 40;
  String f[MAXF];
  int n = 0;
  int start = 0;
  for (int i = 0; i <= (int)csv.length(); i++)
  {
    if (i == (int)csv.length() || csv[i] == ',')
    {
      if (n < MAXF)
        f[n++] = csv.substring(start, i);
      start = i + 1;
    }
  }
  if (n < 9)
    return false;

  out = GpsFix{};
  out.field_count = (uint8_t)n;

  out.run_status = f[0].toInt();
  out.fix_field_present = (f[1].length() > 0);
  out.fix_status = out.fix_field_present ? f[1].toInt() : 0;

  out.utc = f[2];
  out.utc.trim();
  out.lat = f[3].toDouble();
  out.lon = f[4].toDouble();
  out.alt_m = f[5].toDouble();
  out.speed_kmh = f[6].toDouble();
  out.course_deg = f[7].toDouble();
  out.fix_mode = (uint8_t)f[8].toInt();

  // Dina loggar: hdop-ish på index 10, sats_used på index 14
  out.hdop = 999.0f;
  if (n > 10 && f[10].length() > 0)
    out.hdop = f[10].toFloat();

  out.sats_used = 0;
  if (n > 14 && f[14].length() > 0)
    out.sats_used = (uint8_t)f[14].toInt();

  out.fix_age_ms = 0;
  out.start_mode = cmdToStartModeEnum(g_lastStartCmd);

  return true;
}

// -------------------------------------------------------------------------
void gpsInit()
{
  // UART is initialized elsewhere (modem.cpp)
}

bool gpsPowerOn()
{
  if (g_gpsOn)
    return true;

  // Configure output format before power on (harmless if module ignores it)
  atCmdOk("AT+CGNSCFG=0", 2000);

  if (!atCmdOk("AT+CGNSPWR=1", 5000))
  {
    logSystem("GPS: CGNSPWR=1 failed");
    return false;
  }

  const char *startCmd = pickStartCmd();
  g_lastStartCmd = startCmd;

  bool startOk = atCmdOk(String(startCmd), 2000);
  logSystem(String("GPS: start=") + cmdToModeStr(startCmd) + " cmd_ok=" + (startOk ? "1" : "0"));

  // Optional
  atCmdOk("AT+CGNSSEQ=RMC", 2000);

  g_gpsOn = true;
  logSystem("GPS: ON");

  // Reset acquisition tracking
  g_acqStartMs = millis();
  g_acqActive = true;

  g_hasCandidate = false;
  g_stableCount = 0;

  return true;
}

bool gpsPowerOff()
{
  if (!g_gpsOn)
    return true;
  if (!atCmdOk("AT+CGNSPWR=0", 5000))
  {
    logSystem("GPS: CGNSPWR=0 failed");
  }
  g_gpsOn = false;
  g_acqActive = false;
  logSystem("GPS: OFF");
  return true;
}

bool gpsIsOn() { return g_gpsOn; }

bool gpsPollOnce(GpsFix &out)
{
  out = GpsFix{};

  if (!g_gpsOn)
  {
    if (!gpsPowerOn())
      return false;
  }

  String line;
  bool ok = atCmdGetLine("AT+CGNSINF", "+CGNSINF:", line, 2000);
  if (!ok)
    return false;

  // Debug raw (var 10s)
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 10000)
  {
    logSystem(String("GPS: CGNSINF raw=") + line);
    lastDbg = millis();
  }

  if (!parseCgnsinf(line, out))
    return false;

  // Gates
  out.candidate = qualityGate(out);
  out.valid = false;

  if (out.candidate)
  {
    bool stableWithPrev = stabilityGate(out);

    if (!g_hasCandidate)
    {
      g_stableCount = 1;
    }
    else if (stableWithPrev)
    {
      g_stableCount = (uint8_t)min<int>(255, g_stableCount + 1);
    }
    else
    {
      g_stableCount = 1;
    }

    g_lastCandidate = out;
    g_hasCandidate = true;

    out.valid = (g_stableCount >= GPS_STABLE_SAMPLES);
  }
  else
  {
    g_hasCandidate = false;
    g_stableCount = 0;
  }

  // TTFF + start_mode
  out.start_mode = cmdToStartModeEnum(g_lastStartCmd);
  if (g_acqActive && g_acqStartMs != 0)
  {
    out.ttff_s = (uint16_t)((millis() - g_acqStartMs) / 1000UL);
  }

  if (out.valid)
  {
    g_hasFix = true;
    g_lastFix = out;
    g_lastFixAtMs = millis();

    // Parser/format-diagnostik
    logSystem(String("GPS: valid diag fields=") + String(out.field_count) +
              " fixField=" + (out.fix_field_present ? "1" : "0") +
              " fix=" + String(out.fix_status) +
              " hdop=" + String(out.hdop, 1) +
              " sats=" + String(out.sats_used));

    logSystem(String("GPS: FIX OK lat=") + String(out.lat, 6) +
              " lon=" + String(out.lon, 6) +
              " alt=" + String(out.alt_m, 1) +
              " spd=" + String(out.speed_kmh, 1) +
              " hdop=" + String(out.hdop, 1) +
              " sats=" + String(out.sats_used) +
              " ttff_s=" + String(out.ttff_s) +
              " start=" + String(cmdToModeStr(g_lastStartCmd)));

    // Punkt 3: markera lågt alt som “misstänkt” men stoppa inte fixen
    if (out.speed_kmh < 1.0f && out.alt_m < -20.0)
    {
      logSystem(String("GPS: WARN alt looks low (") + String(out.alt_m, 1) +
                " m) - ignoring alt for decisions recommended");
    }
  }

  return true;
}

bool gpsGetFixWait(GpsFix &out, uint32_t maxWaitMs)
{
  uint32_t startMs = millis();
  if (!gpsPowerOn())
    return false;

  while (millis() - startMs < maxWaitMs)
  {
    GpsFix tmp;
    bool ok = gpsPollOnce(tmp);
    if (ok && tmp.valid)
    {
      out = tmp;
      return true;
    }
    delay(1000);
  }

  if (g_hasFix)
  {
    out = g_lastFix;
    out.valid = false;
    out.fix_age_ms = millis() - g_lastFixAtMs;
    out.start_mode = cmdToStartModeEnum(g_lastStartCmd);
  }
  return false;
}

bool gpsHasLastFix() { return g_hasFix; }
GpsFix gpsLastFix() { return g_lastFix; }
uint32_t gpsLastFixAtMs() { return g_lastFixAtMs; }
