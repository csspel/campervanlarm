#pragma once
#include <Arduino.h>

// GPS-fix från SIM7080 (AT+CGNSINF)
// valid=true betyder: passerat kvalitetsgate + stabilitetsgate (N prover i rad)
// candidate=true betyder: ser rimligt ut men inte stabilt än.
struct GpsFix
{
  bool valid = false;
  bool candidate = false;

  String utc; // YYYYMMDDHHMMSS.sss (from CGNSINF)
  double lat = 0.0;
  double lon = 0.0;
  double alt_m = 0.0;
  double speed_kmh = 0.0;
  double course_deg = 0.0;

  uint32_t fix_age_ms = 0; // age since fix was read
  uint8_t fix_mode = 0;    // module dependent

  // Diagnostik från CGNSINF
  float hdop = 999.0f;
  uint8_t sats_used = 0;
  bool fix_field_present = false;
  int fix_status = 0;
  int run_status = 0;
  uint8_t field_count = 0;

  // TTFF / start mode (för MQTT + logg)
  uint16_t ttff_s = 0;    // time-to-first-fix (sek) i aktuell acquisition
  uint8_t start_mode = 0; // 1=COLD, 2=WARM, 3=HOT
};

void gpsInit();
bool gpsPowerOn();
bool gpsPowerOff();
bool gpsIsOn();

// Blockerande helper (används inte i pipeline, men finns kvar)
bool gpsGetFixWait(GpsFix &out, uint32_t maxWaitMs = 30000);

// Non-blocking poll (används i pipeline)
bool gpsPollOnce(GpsFix &out);

bool gpsHasLastFix();
GpsFix gpsLastFix();
uint32_t gpsLastFixAtMs();
