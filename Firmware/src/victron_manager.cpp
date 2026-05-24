#include "victron_manager.h"

#include "config.h"
#include "logging.h"
#include "VictronBLE.h"
#include "time_manager.h"

#include <math.h>

#if VICTRON_BLE_ENABLED

static VictronBLE g_victronBle;
static bool g_victronConfigured = false;
static bool g_publishPending = false;
static uint32_t g_nextScanAtMs = 0;
static uint32_t g_lastScanStartMs = 0;
static uint32_t g_lastScanEndMs = 0;
static uint32_t g_scanCountBoot = 0;
static uint32_t g_deviceUpdateCountBoot = 0;
static uint32_t g_scanSmartshuntUpdates = 0;
static uint32_t g_scanSmartsolarUpdates = 0;
static uint32_t g_scanOrionUpdates = 0;

struct VictronLatestData
{
  bool smartshunt_valid = false;
  bool smartsolar_valid = false;
  bool orion_valid = false;

  uint32_t smartshunt_last_seen_ms = 0;
  uint32_t smartsolar_last_seen_ms = 0;
  uint32_t orion_last_seen_ms = 0;

  int smartshunt_rssi = -127;
  int smartsolar_rssi = -127;
  int orion_rssi = -127;

  float soc_pct = NAN;
  float battery_voltage_v = NAN;
  float battery_current_a = NAN;
  float consumed_ah = NAN;
  uint16_t time_to_go_min = 0;

  float solar_battery_voltage_v = NAN;
  float solar_battery_current_a = NAN;
  float solar_pv_power_w = NAN;
  float solar_yield_today_kwh = NAN;
  uint16_t solar_yield_today_wh = 0;
  uint8_t solar_state_code = 255;
  uint8_t solar_error_code = 255;

  float orion_input_voltage_v = NAN;
  float orion_output_voltage_v = NAN;
  float orion_output_current_a = NAN;
  uint8_t orion_state_code = 255;
  uint8_t orion_error_code = 255;
};

static VictronLatestData g_victron;

static String chargerStateToText(uint8_t state)
{
  switch (state)
  {
  case CHARGER_OFF:
    return "Off";
  case CHARGER_LOW_POWER:
    return "Low power";
  case CHARGER_FAULT:
    return "Fault";
  case CHARGER_BULK:
    return "Bulk";
  case CHARGER_ABSORPTION:
    return "Absorption";
  case CHARGER_FLOAT:
    return "Float";
  case CHARGER_STORAGE:
    return "Storage";
  case CHARGER_EQUALIZE:
    return "Equalize";
  case CHARGER_INVERTING:
    return "Inverting";
  case CHARGER_POWER_SUPPLY:
    return "Power supply";
  case CHARGER_EXTERNAL_CONTROL:
    return "External control";
  default:
    return String(state);
  }
}

static bool isFresh(bool valid, uint32_t lastSeenMs, uint32_t nowMs)
{
  return valid && lastSeenMs != 0 && (uint32_t)(nowMs - lastSeenMs) < VICTRON_FRESH_TIMEOUT_MS;
}

static void appendFloatOrNull(String &payload, const char *key, float value, uint8_t decimals, bool commaBefore = true)
{
  if (commaBefore)
    payload += ",";
  payload += "\"";
  payload += key;
  payload += "\":";

  if (isnan(value) || isinf(value))
  {
    payload += "null";
  }
  else
  {
    payload += String(value, (unsigned int)decimals);
  }
}

static void onVictronData(const VictronDevice *device)
{
  if (!device)
    return;

  const uint32_t nowMs = millis();
  g_deviceUpdateCountBoot++;
  g_publishPending = true;

  switch (device->deviceType)
  {
  case DEVICE_TYPE_SOLAR_CHARGER:
  {
    g_scanSmartsolarUpdates++;
    const VictronSolarData &s = device->solar;

    g_victron.smartsolar_valid = true;
    g_victron.smartsolar_last_seen_ms = nowMs;
    g_victron.smartsolar_rssi = device->rssi;
    g_victron.solar_battery_voltage_v = s.batteryVoltage;
    g_victron.solar_battery_current_a = s.batteryCurrent;
    g_victron.solar_pv_power_w = s.panelPower;
    g_victron.solar_yield_today_wh = s.yieldToday;
    g_victron.solar_yield_today_kwh = ((float)s.yieldToday) / 1000.0f;
    g_victron.solar_state_code = s.chargeState;
    g_victron.solar_error_code = s.errorCode;

    logSystemf("VICTRON: SmartSolar %.2fV %.2fA PV=%.0fW yield=%.2fkWh state=%s rssi=%d",
               g_victron.solar_battery_voltage_v,
               g_victron.solar_battery_current_a,
               g_victron.solar_pv_power_w,
               g_victron.solar_yield_today_kwh,
               chargerStateToText(g_victron.solar_state_code).c_str(),
               g_victron.smartsolar_rssi);
    break;
  }

  case DEVICE_TYPE_BATTERY_MONITOR:
  {
    g_scanSmartshuntUpdates++;
    const VictronBatteryData &b = device->battery;

    g_victron.smartshunt_valid = true;
    g_victron.smartshunt_last_seen_ms = nowMs;
    g_victron.smartshunt_rssi = device->rssi;
    g_victron.battery_voltage_v = b.voltage;
    g_victron.battery_current_a = b.current;
    g_victron.soc_pct = b.soc;
    g_victron.consumed_ah = b.consumedAh;
    g_victron.time_to_go_min = b.remainingMinutes;

    logSystemf("VICTRON: SmartShunt %.2fV %.3fA SOC=%.1f%% consumed=%.1fAh rssi=%d",
               g_victron.battery_voltage_v,
               g_victron.battery_current_a,
               g_victron.soc_pct,
               g_victron.consumed_ah,
               g_victron.smartshunt_rssi);
    break;
  }

  case DEVICE_TYPE_DCDC_CONVERTER:
  {
    g_scanOrionUpdates++;
    const VictronDCDCData &d = device->dcdc;

    g_victron.orion_valid = true;
    g_victron.orion_last_seen_ms = nowMs;
    g_victron.orion_rssi = device->rssi;
    g_victron.orion_input_voltage_v = d.inputVoltage;
    g_victron.orion_output_voltage_v = d.outputVoltage;
    g_victron.orion_output_current_a = d.outputCurrent;
    g_victron.orion_state_code = d.chargeState;
    g_victron.orion_error_code = d.errorCode;

    logSystemf("VICTRON: Orion XS in=%.2fV out=%.2fV current=%.2fA state=%u rssi=%d",
               g_victron.orion_input_voltage_v,
               g_victron.orion_output_voltage_v,
               g_victron.orion_output_current_a,
               (unsigned)g_victron.orion_state_code,
               g_victron.orion_rssi);
    break;
  }

  default:
    break;
  }
}

void victronManagerInit()
{
  g_nextScanAtMs = millis() + 60000UL; // första testscan efter boot, inte direkt under uppstart
  g_publishPending = false;
  logSystem("VICTRON: manager init");
}

static bool configureVictronBle(uint32_t scanSeconds)
{
  g_victronBle.setDebug(false);
  g_victronBle.setCallback(onVictronData);
  g_victronBle.setMinInterval(1000);

  if (!g_victronBle.begin(scanSeconds))
  {
    logSystem("VICTRON: BLE begin failed");
    return false;
  }

  // Enhetslistan ligger kvar i VictronBLE-objektet även efter end().
  // Lägg därför bara till enheter första gången.
  if (g_victronConfigured)
  {
    return g_victronBle.getDeviceCount() > 0;
  }

  bool any = false;

  if (strlen(VICTRON_MAC_1) > 0 && strlen(VICTRON_KEY_1) == 32)
  {
    bool ok = g_victronBle.addDevice(VICTRON_NAME_1, VICTRON_MAC_1, VICTRON_KEY_1, DEVICE_TYPE_DCDC_CONVERTER);
    logSystem(String("VICTRON: add ") + VICTRON_NAME_1 + " " + (ok ? "OK" : "FAILED"));
    any = any || ok;
  }

  if (strlen(VICTRON_MAC_2) > 0 && strlen(VICTRON_KEY_2) == 32)
  {
    bool ok = g_victronBle.addDevice(VICTRON_NAME_2, VICTRON_MAC_2, VICTRON_KEY_2, DEVICE_TYPE_SOLAR_CHARGER);
    logSystem(String("VICTRON: add ") + VICTRON_NAME_2 + " " + (ok ? "OK" : "FAILED"));
    any = any || ok;
  }

  if (strlen(VICTRON_MAC_3) > 0 && strlen(VICTRON_KEY_3) == 32)
  {
    bool ok = g_victronBle.addDevice(VICTRON_NAME_3, VICTRON_MAC_3, VICTRON_KEY_3, DEVICE_TYPE_BATTERY_MONITOR);
    logSystem(String("VICTRON: add ") + VICTRON_NAME_3 + " " + (ok ? "OK" : "FAILED"));
    any = any || ok;
  }

  g_victronConfigured = any;
  logSystemf("VICTRON: configured devices=%u", (unsigned)g_victronBle.getDeviceCount());
  return any;
}

bool victronManagerDue(uint32_t nowMs, const ProfileConfig &profile)
{
  if (!profile.victronBleEnabled || profile.victronBleIntervalMs == 0 || profile.victronBleScanSeconds == 0)
    return false;

  return (int32_t)(nowMs - g_nextScanAtMs) >= 0;
}

bool victronManagerRunScanOnce(uint32_t nowMs, uint32_t scanSeconds)
{
  g_lastScanStartMs = nowMs;
  g_lastScanEndMs = 0;
  g_scanCountBoot++;
  g_scanSmartshuntUpdates = 0;
  g_scanSmartsolarUpdates = 0;
  g_scanOrionUpdates = 0;
  g_publishPending = true; // publicera även scanstatus om inget hittades

  logSystemf("VICTRON: scan start seconds=%lu heap_free=%lu",
             (unsigned long)scanSeconds,
             (unsigned long)ESP.getFreeHeap());

  bool ok = configureVictronBle(scanSeconds);
  if (ok && g_victronConfigured)
  {
    g_victronBle.resetScanStats();
    ok = g_victronBle.scanOnce(scanSeconds);
  }

  const uint32_t advSeen = g_victronBle.getScanAdvSeen();
  const uint32_t knownSeen = g_victronBle.getScanKnownSeen();
  const uint32_t unknownSeen = g_victronBle.getScanUnknownSeen();
  const uint32_t parseFailSeen = g_victronBle.getScanParseFailSeen();
  const uint32_t parseSuccessSeen = g_victronBle.getScanParseSuccessSeen();

  g_victronBle.end();

  const uint32_t doneMs = millis();
  g_lastScanEndMs = doneMs;
  g_nextScanAtMs = doneMs + currentProfile().victronBleIntervalMs;

  logSystemf("VICTRON: scan summary configured=%u adv=%lu known=%lu unknown=%lu parse_ok=%lu parse_fail=%lu shunt=%lu solar=%lu orion=%lu",
             (unsigned)g_victronBle.getDeviceCount(),
             (unsigned long)advSeen,
             (unsigned long)knownSeen,
             (unsigned long)unknownSeen,
             (unsigned long)parseSuccessSeen,
             (unsigned long)parseFailSeen,
             (unsigned long)g_scanSmartshuntUpdates,
             (unsigned long)g_scanSmartsolarUpdates,
             (unsigned long)g_scanOrionUpdates);

  logSystemf("VICTRON: scan done ok=%d duration_ms=%lu updates_boot=%lu heap_free=%lu",
             ok ? 1 : 0,
             (unsigned long)(doneMs - nowMs),
             (unsigned long)g_deviceUpdateCountBoot,
             (unsigned long)ESP.getFreeHeap());

  return ok;
}

bool victronManagerPublishPending()
{
  return g_publishPending;
}

void victronManagerClearPublishPending()
{
  g_publishPending = false;
}

String victronManagerBuildStateJson()
{
  const uint32_t nowMs = millis();
  const bool smartshuntFresh = isFresh(g_victron.smartshunt_valid, g_victron.smartshunt_last_seen_ms, nowMs);
  const bool smartsolarFresh = isFresh(g_victron.smartsolar_valid, g_victron.smartsolar_last_seen_ms, nowMs);
  const bool orionFresh = isFresh(g_victron.orion_valid, g_victron.orion_last_seen_ms, nowMs);

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"type\":\"VICTRON\",";
  payload += "\"timestamp\":\"" + timeIsoUtc() + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeIsValid() ? "true" : "false") + ",";
  payload += "\"profile\":\"" + String(currentProfile().name) + "\",";
  payload += "\"uptime_s\":" + String(nowMs / 1000) + ",";
  payload += "\"scan_count_boot\":" + String(g_scanCountBoot) + ",";
  payload += "\"device_update_count_boot\":" + String(g_deviceUpdateCountBoot) + ",";
  payload += "\"last_scan_age_s\":" + String(g_lastScanEndMs ? (int)((nowMs - g_lastScanEndMs) / 1000) : -1) + ",";

  payload += "\"smartshunt_valid\":" + String(g_victron.smartshunt_valid ? "true" : "false") + ",";
  payload += "\"smartshunt_fresh\":" + String(smartshuntFresh ? "true" : "false") + ",";
  payload += "\"smartshunt_seen_s_ago\":" + String(g_victron.smartshunt_valid ? (int)((nowMs - g_victron.smartshunt_last_seen_ms) / 1000) : -1) + ",";
  payload += "\"smartshunt_rssi\":" + String(g_victron.smartshunt_rssi) + ",";

  payload += "\"smartsolar_valid\":" + String(g_victron.smartsolar_valid ? "true" : "false") + ",";
  payload += "\"smartsolar_fresh\":" + String(smartsolarFresh ? "true" : "false") + ",";
  payload += "\"smartsolar_seen_s_ago\":" + String(g_victron.smartsolar_valid ? (int)((nowMs - g_victron.smartsolar_last_seen_ms) / 1000) : -1) + ",";
  payload += "\"smartsolar_rssi\":" + String(g_victron.smartsolar_rssi) + ",";

  payload += "\"orion_valid\":" + String(g_victron.orion_valid ? "true" : "false") + ",";
  payload += "\"orion_fresh\":" + String(orionFresh ? "true" : "false") + ",";
  payload += "\"orion_seen_s_ago\":" + String(g_victron.orion_valid ? (int)((nowMs - g_victron.orion_last_seen_ms) / 1000) : -1) + ",";
  payload += "\"orion_rssi\":" + String(g_victron.orion_rssi);

  appendFloatOrNull(payload, "soc_pct", g_victron.soc_pct, 1);
  appendFloatOrNull(payload, "battery_voltage_v", g_victron.battery_voltage_v, 2);
  appendFloatOrNull(payload, "battery_current_a", g_victron.battery_current_a, 3);
  appendFloatOrNull(payload, "consumed_ah", g_victron.consumed_ah, 1);
  payload += ",\"time_to_go_min\":" + String(g_victron.time_to_go_min);

  appendFloatOrNull(payload, "solar_battery_voltage_v", g_victron.solar_battery_voltage_v, 2);
  appendFloatOrNull(payload, "solar_battery_current_a", g_victron.solar_battery_current_a, 2);
  appendFloatOrNull(payload, "solar_pv_power_w", g_victron.solar_pv_power_w, 0);
  payload += ",\"solar_yield_today_wh\":" + String(g_victron.solar_yield_today_wh);
  appendFloatOrNull(payload, "solar_yield_today_kwh", g_victron.solar_yield_today_kwh, 3);
  payload += ",\"solar_state_code\":" + String(g_victron.solar_state_code);
  payload += ",\"solar_state\":\"" + chargerStateToText(g_victron.solar_state_code) + "\"";
  payload += ",\"solar_error_code\":" + String(g_victron.solar_error_code);

  appendFloatOrNull(payload, "orion_input_voltage_v", g_victron.orion_input_voltage_v, 2);
  appendFloatOrNull(payload, "orion_output_voltage_v", g_victron.orion_output_voltage_v, 2);
  appendFloatOrNull(payload, "orion_output_current_a", g_victron.orion_output_current_a, 2);
  payload += ",\"orion_state_code\":" + String(g_victron.orion_state_code);
  payload += ",\"orion_error_code\":" + String(g_victron.orion_error_code);

  payload += "}";
  return payload;
}

#else

void victronManagerInit() {}
bool victronManagerDue(uint32_t, const ProfileConfig &) { return false; }
bool victronManagerRunScanOnce(uint32_t, uint32_t) { return false; }
bool victronManagerPublishPending() { return false; }
void victronManagerClearPublishPending() {}
String victronManagerBuildStateJson() { return "{}"; }

#endif
