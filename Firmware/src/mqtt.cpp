#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "ext_gnss.h"
#include "modem.h"
#include "profiles.h"
#include "time_manager.h"
#include "victron_manager.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <Preferences.h>

// ============================================================
// MQTT state
// ------------------------------------------------------------
// netClient:
//   Pekar på nätverksklienten som går via modemet.
//
// mqttClientInstance:
//   Den faktiska PubSubClient-instansen.
//
// mqttClient:
//   Pekare till instansen.
//
// msgCounter:
//   Enkel räknare för utgående msg_id.
//
// lastHandledProfileChangeId:
//   Senast hanterade profile_change_id under denna boot.
//   Används för att undvika dubbelhantering av retained state
//   som spelas upp igen under samma boot.
//
// desiredProfileSeenThisConnect:
//   Sätts true när vi under aktuell MQTT-anslutning har sett
//   ett desired-profile-meddelande.
//   Det används av pipeline för att veta om retained profil
//   hunnit komma efter subscribe.
// ============================================================
static Client *netClient = nullptr;
static PubSubClient mqttClientInstance;
static PubSubClient *mqttClient = nullptr;
static WiFiClient wifiClientInstance;
static uint32_t msgCounter = 0;

// Senaste nätstatus. Pipeline uppdaterar dessa när den väljer SIM/WiFi
// och när MQTT connect lyckas/misslyckas. mqttPublishNetStatus() skickar dem till HA.
static String g_activeLink = "SIM";
static bool g_wifiOk = false;
static bool g_simOk = true;
static bool g_mqttOk = false;
static int g_wifiRssi = 0;
static int g_modemRssi = -1;
static String g_lastNetFailReason = "NONE";
static uint32_t lastHandledProfileChangeId = 0;
static bool desiredProfileSeenThisConnect = false;
static bool g_profileAckPending = false;
static uint32_t g_profileAckPendingId = 0;
static String g_profileAckPendingStatus;
static String g_profileAckPendingDetail;

// ============================================================
// Network mode state
// ------------------------------------------------------------
// Första införandet: vi tar emot önskat nätläge från HA, ACK:ar
// och publicerar status. Själva WiFi/SIM-fallbacken byggs i nästa steg.
// ============================================================
static uint32_t lastHandledNetModeChangeId = 0;
static String g_desiredNetMode = "SIM_PRIMARY";
static uint32_t g_netModeChangeId = 0;
static bool g_netModeLoadedFromNvs = false;
static bool g_netModeSwitchRequested = false;

static Preferences g_netPrefs;

static bool mqttModeWantsWifi(const String &mode)
{
  String m = mode;
  m.toUpperCase();
  return m == "WIFI_ONLY" || m == "WIFI_PRIMARY" || m == "AUTO";
}

static bool mqttModeWantsSim(const String &mode)
{
  String m = mode;
  m.toUpperCase();
  return m == "SIM_ONLY" || m == "SIM_PRIMARY";
}

static void mqttLoadNetModeFromNvs()
{
  if (g_netModeLoadedFromNvs)
    return;

  g_netModeLoadedFromNvs = true;

  if (!g_netPrefs.begin("van_net", false))
  {
    logSystem("MQTT: net mode NVS open failed, using default SIM_PRIMARY");
    return;
  }

  String mode = g_netPrefs.getString("mode", "SIM_PRIMARY");
  uint32_t id = g_netPrefs.getUInt("change_id", 0);
  mode.toUpperCase();

  if (mode == "SIM_PRIMARY" || mode == "WIFI_PRIMARY" ||
      mode == "SIM_ONLY" || mode == "WIFI_ONLY" || mode == "AUTO")
  {
    g_desiredNetMode = mode;
    g_netModeChangeId = id;
    lastHandledNetModeChangeId = id;
    logSystem("MQTT: loaded net_mode from NVS mode=" + g_desiredNetMode +
              " change_id=" + String(g_netModeChangeId));
  }
  else
  {
    logSystem("MQTT: invalid net_mode in NVS, using default SIM_PRIMARY");
  }
}

static void mqttSaveNetModeToNvs()
{
  mqttLoadNetModeFromNvs();
  g_netPrefs.putString("mode", g_desiredNetMode);
  g_netPrefs.putUInt("change_id", g_netModeChangeId);
  logSystem("MQTT: saved net_mode to NVS mode=" + g_desiredNetMode +
            " change_id=" + String(g_netModeChangeId));
}

// Hook från pipeline som används när HA/server kvitterar PIR-event.
extern void pipelineOnPirAck(uint32_t eventId);

// ============================================================
// Minimal JSON helpers
// ============================================================

// Läs ut en sträng från JSON, t.ex. key="desired_profile".
static String jsonGetString(const String &json, const char *key)
{
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0)
    return "";

  i += k.length();

  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t'))
    i++;

  if (i >= (int)json.length() || json[i] != '"')
    return "";

  i++;

  int j = json.indexOf('"', i);
  if (j < 0)
    return "";

  return json.substring(i, j);
}

// Läs ut ett osignerat heltal från JSON.
static uint32_t jsonGetUInt(const String &json, const char *key)
{
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0)
    return 0;

  i += k.length();

  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t'))
    i++;

  if (i < (int)json.length() && json[i] == '"')
    i++;

  uint32_t val = 0;
  while (i < (int)json.length() && isDigit(json[i]))
  {
    val = val * 10 + (json[i] - '0');
    i++;
  }

  return val;
}

// ============================================================
// Internal helpers
// ============================================================

// Returnerar aktuell tidskälla som text till JSON.
static const char *mqttTimeSourceText()
{
  switch (timeGetSource())
  {
  case TimeSource::MODEM:
    return "MODEM";
  case TimeSource::NTP:
    return "NTP";
  default:
    return "NONE";
  }
}

// Bygger JSON-fält som återkommer i många payloads.
static String mqttBuildCommonJsonFields(const char *msgType, bool includeMsgId)
{
  String payload;

  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";

  if (includeMsgId)
  {
    payload += "\"msg_id\":\"" + String(++msgCounter) + "\",";
  }

  payload += "\"type\":\"" + String(msgType) + "\",";
  payload += "\"timestamp\":\"" + timeIsoUtc() + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeIsValid() ? "true" : "false") + ",";
  payload += "\"time_source\":\"" + String(mqttTimeSourceText()) + "\",";
  payload += "\"date_local\":\"" + timeDateLocal() + "\",";
  payload += "\"time_local\":\"" + timeClockLocal() + "\",";
  payload += "\"profile\":\"" + String(currentProfile().name) + "\"";

  return payload;
}

// Publicera ACK till HA/server på ack-topic.
//
// För profiländringar skickar vi nu primärt profile_change_id,
// men skickar även ack_msg_id med samma värde för bakåtkompatibilitet.
static bool mqttPublishAck(uint32_t profileChangeId, const char *status, const char *detail = "")
{
  if (!mqttClient || !mqttClient->connected())
    return false;

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"type\":\"ACK\",";
  payload += "\"profile_change_id\":" + String(profileChangeId) + ",";
  payload += "\"ack_msg_id\":" + String(profileChangeId) + ",";
  payload += "\"status\":\"" + String(status) + "\",";
  payload += "\"detail\":\"" + String(detail) + "\",";
  payload += "\"profile\":\"" + String(currentProfile().name) + "\",";

#ifdef FW_VERSION
  payload += "\"fw\":\"" + String(FW_VERSION) + "\",";
#endif

  payload += "\"epoch_utc\":" + String(timeEpochUtc());
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_ACK, payload.c_str(), false);
  logSystem(String("MQTT: ACK publish ") + (ok ? "OK" : "FAILED") + " payload=" + payload);
  return ok;
}

bool mqttHasPendingProfileAck()
{
  return g_profileAckPending;
}

bool mqttPublishPendingProfileAck()
{
  if (!g_profileAckPending)
  {
    return true;
  }

  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish pending profile ACK, not connected");
    return false;
  }

  bool ok = mqttPublishAck(g_profileAckPendingId,
                           g_profileAckPendingStatus.c_str(),
                           g_profileAckPendingDetail.c_str());

  if (!ok)
  {
    return false;
  }

  g_profileAckPending = false;
  g_profileAckPendingId = 0;
  g_profileAckPendingStatus = "";
  g_profileAckPendingDetail = "";

  return true;
}

static void mqttQueueProfileAck(uint32_t profileChangeId, const char *status, const char *detail = "")
{
  g_profileAckPending = true;
  g_profileAckPendingId = profileChangeId;
  g_profileAckPendingStatus = String(status);
  g_profileAckPendingDetail = String(detail);

  logSystem("MQTT: queued profile ACK id=" + String(profileChangeId) +
            " status=" + g_profileAckPendingStatus +
            " detail=" + g_profileAckPendingDetail);
}

static bool mqttIsValidNetMode(const String &mode)
{
  return mode == "SIM_PRIMARY" ||
         mode == "WIFI_PRIMARY" ||
         mode == "SIM_ONLY" ||
         mode == "WIFI_ONLY" ||
         mode == "AUTO";
}

static bool mqttNetModeNeedsWifi(const String &mode)
{
  return mqttModeWantsWifi(mode);
}

static bool mqttPublishNetModeAck(uint32_t changeId,
                                  bool accepted,
                                  const char *status,
                                  const char *detail = "")
{
  if (!mqttClient || !mqttClient->connected())
    return false;

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"type\":\"NET_MODE_ACK\",";
  payload += "\"accepted\":" + String(accepted ? "true" : "false") + ",";
  payload += "\"net_mode_change_id\":" + String(changeId) + ",";
  payload += "\"change_id\":" + String(changeId) + ",";
  payload += "\"net_mode\":\"" + g_desiredNetMode + "\",";
  payload += "\"status\":\"" + String(status) + "\",";
  payload += "\"detail\":\"" + String(detail) + "\",";
  payload += "\"active_link\":\"" + g_activeLink + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc());
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_ACK_NET_MODE, payload.c_str(), false);
  logSystem(String("MQTT: net_mode ACK publish ") + (ok ? "OK" : "FAILED") +
            " payload=" + payload);
  return ok;
}

static void mqttHandleDesiredNetModeMessage(const String &msg)
{
  uint32_t changeId = jsonGetUInt(msg, "net_mode_change_id");

  // HA skickar även generiskt change_id. Använd det som fallback.
  if (changeId == 0)
  {
    changeId = jsonGetUInt(msg, "change_id");
  }

  String netMode = jsonGetString(msg, "net_mode");

  if (changeId == 0)
  {
    mqttPublishNetModeAck(0, false, "ERROR", "missing_net_mode_change_id");
    return;
  }

  if (changeId == lastHandledNetModeChangeId)
  {
    mqttPublishNetModeAck(changeId, true, "DUPLICATE_IGNORED", "same_net_mode_change_id");
    return;
  }

  if (netMode.length() == 0)
  {
    mqttPublishNetModeAck(changeId, false, "ERROR", "missing_net_mode");
    return;
  }

  netMode.toUpperCase();

  if (!mqttIsValidNetMode(netMode))
  {
    mqttPublishNetModeAck(changeId, false, "ERROR", "unknown_net_mode");
    return;
  }

  mqttLoadNetModeFromNvs();

  const String oldMode = g_desiredNetMode;
  const String oldLink = g_activeLink;

  lastHandledNetModeChangeId = changeId;
  g_desiredNetMode = netMode;
  g_netModeChangeId = changeId;

  mqttSaveNetModeToNvs();

  // Om HA byter mellan SIM- och WiFi-läge medan vi redan är uppkopplade
  // behöver pipeline koppla ner och ansluta på nytt med rätt länk.
  bool wantsWifiNow = mqttModeWantsWifi(g_desiredNetMode);
  bool activeIsWifi = (oldLink == "WIFI");
  g_netModeSwitchRequested = (oldMode != g_desiredNetMode) && (wantsWifiNow != activeIsWifi);

  const char *detail = g_netModeSwitchRequested
                           ? "net_mode_stored_reconnect_requested"
                           : "net_mode_stored";

  mqttPublishNetModeAck(changeId, true, "OK", detail);

  // Publicera status direkt så HA-UI uppdateras utan att vänta på nästa publish-cykel.
  mqttPublishNetStatus();
}

// Hantera desired-profile payload.
//
// fromLegacyDownlink:
//   true  = meddelandet kom från gamla cmd/downlink
//   false = meddelandet kom från nya state/desired_profile
static void mqttHandleDesiredProfileMessage(const String &msg, bool fromLegacyDownlink)
{
  // Nya namnet
  uint32_t profileChangeId = jsonGetUInt(msg, "profile_change_id");

  // Fallback till gamla namnet under migration
  if (profileChangeId == 0)
  {
    profileChangeId = jsonGetUInt(msg, "ack_msg_id");
  }

  String desiredProfile = jsonGetString(msg, "desired_profile");

  if (profileChangeId == 0)
  {
    mqttPublishAck(0, "ERROR", "missing_profile_change_id");
    return;
  }

  // Dedupe inom samma boot
  if (profileChangeId == lastHandledProfileChangeId)
  {
    mqttPublishAck(profileChangeId, "DUPLICATE_IGNORED", "same_profile_change_id");
    return;
  }

  lastHandledProfileChangeId = profileChangeId;

  if (desiredProfile.length() == 0)
  {
    mqttPublishAck(profileChangeId, "ERROR", "missing_desired_profile");
    return;
  }

  ProfileId pid;
  if (!profileFromString(desiredProfile, pid))
  {
    mqttPublishAck(profileChangeId, "ERROR", "unknown_profile");
    return;
  }

  // Om vi redan är i samma profil:
  // ACK:a ändå som OK så HA vet att state stämmer.
  if (String(currentProfile().name) == desiredProfile)
  {
    mqttPublishAck(profileChangeId,
                   "OK",
                   fromLegacyDownlink ? "profile_already_set_legacy" : "profile_already_set");

    // Publicera alive direkt så HA snabbt ser en bekräftelse
    // även om profil inte behövde ändras.
    mqttPublishAlive();
    return;
  }

  // Queuea ACK först så pipeline kan publicera den i ordnad publish-cykel
  mqttQueueProfileAck(profileChangeId,
                      "OK",
                      fromLegacyDownlink ? "profile_set_from_legacy" : "profile_set");

  // Applicera ny profil.
  // pipelineOnProfileChanged() ser då till att hålla anslutningen uppe
  // tills publish hunnit gå med nya profilen.
  setProfile(pid);
}

// ============================================================
// MQTT callback
// ============================================================
static void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  String t(topic);
  String msg;
  msg.reserve(length);

  for (unsigned int i = 0; i < length; i++)
  {
    msg += (char)payload[i];
  }

  msg.trim();

  if (msg.length() == 0)
  {
    return;
  }

  logSystem("MQTT: RX topic=" + t + " payload=" + msg);

  // ----------------------------------------------------------
  // Topic: MQTT_TOPIC_CMD_ACK
  // ----------------------------------------------------------
  // PIR-ACK från HA/server
  // ----------------------------------------------------------
  if (t == MQTT_TOPIC_CMD_ACK)
  {
    String typ = jsonGetString(msg, "type");
    uint32_t eventId = jsonGetUInt(msg, "pir_event_id");

    // Tolerant fallback
    if (eventId == 0)
    {
      eventId = jsonGetUInt(msg, "event_id");
    }

    if ((typ.length() == 0 || typ == "PIR_ACK") && eventId != 0)
    {
      pipelineOnPirAck(eventId);
      logSystem("MQTT: PIR_ACK received event_id=" + String(eventId));
    }

    return;
  }

  // ----------------------------------------------------------
  // Topic: MQTT_TOPIC_NET_MODE_DESIRED
  // ----------------------------------------------------------
  if (t == MQTT_TOPIC_NET_MODE_DESIRED)
  {
    mqttHandleDesiredNetModeMessage(msg);
    return;
  }

  // ----------------------------------------------------------
  // Topic: MQTT_TOPIC_DESIRED_PROFILE
  // ----------------------------------------------------------
  if (t == MQTT_TOPIC_DESIRED_PROFILE)
  {
    // Detta markerar att vi verkligen sett en desired-profile payload
    // under aktuell MQTT-session.
    desiredProfileSeenThisConnect = true;

    mqttHandleDesiredProfileMessage(msg, false);
    return;
  }

  // ----------------------------------------------------------
  // Topic: MQTT_TOPIC_DOWNLINK
  // ----------------------------------------------------------
  // Under migration tolererar vi fortfarande desired_profile här.
  // På sikt ska denna topic användas för rena engångskommandon.
  // ----------------------------------------------------------
  if (t == MQTT_TOPIC_DOWNLINK)
  {
    String desiredProfile = jsonGetString(msg, "desired_profile");

    if (desiredProfile.length() > 0)
    {
      desiredProfileSeenThisConnect = true;
      mqttHandleDesiredProfileMessage(msg, true);
      return;
    }

    logSystem("MQTT: cmd/downlink received but no supported command found");
    return;
  }
}

// ============================================================
// Public API
// ============================================================

void mqttUseSimClient()
{
  if (!mqttClient)
  {
    mqttSetup();
  }

  netClient = &modemGetClient();
  mqttClient->setClient(*netClient);
  g_activeLink = "SIM";
  logSystem("MQTT: selected network client = SIM/modem");
}

void mqttUseWifiClient()
{
  if (!mqttClient)
  {
    mqttSetup();
  }

  netClient = &wifiClientInstance;
  mqttClient->setClient(*netClient);
  g_activeLink = "WIFI";
  logSystem("MQTT: selected network client = WIFI");
}

void mqttSetNetStatus(const char *activeLink,
                      bool wifiOk,
                      bool simOk,
                      bool mqttOk,
                      int wifiRssi,
                      int modemRssi,
                      const char *lastFailReason)
{
  g_activeLink = activeLink ? activeLink : "NONE";
  g_wifiOk = wifiOk;
  g_simOk = simOk;
  g_mqttOk = mqttOk;
  g_wifiRssi = wifiRssi;
  g_modemRssi = modemRssi;
  g_lastNetFailReason = (lastFailReason && strlen(lastFailReason) > 0) ? lastFailReason : "NONE";
}

const char *mqttGetActiveLink()
{
  return g_activeLink.c_str();
}

void mqttSetup()
{
  mqttLoadNetModeFromNvs();

  if (!netClient)
  {
    netClient = &modemGetClient();
  }

  if (!mqttClient)
  {
    mqttClient = &mqttClientInstance;

    mqttClient->setClient(*netClient);
    mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient->setCallback(mqttCallback);

    // GPS + JSON kräver lite större buffer.
    mqttClient->setBufferSize(2048);

    // Keepalive och socket-timeout
    mqttClient->setKeepAlive(30);
    mqttClient->setSocketTimeout(15);
  }
}

bool mqttConnect()
{
  if (!mqttClient)
  {
    mqttSetup();
  }

  logSystem("MQTT: connecting to broker");
  logSystem(String("MQTT: host=") + MQTT_BROKER_HOST + ":" + String(MQTT_BROKER_PORT));

  mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  bool ok = false;

  if (strlen(MQTT_USERNAME) > 0)
  {
    ok = mqttClient->connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  }
  else
  {
    ok = mqttClient->connect(MQTT_CLIENT_ID);
  }

  if (!ok)
  {
    g_mqttOk = false;
    g_lastNetFailReason = "MQTT_CONNECT_FAILED";
    logSystem("MQTT: connect FAILED, rc=" + String(mqttClient->state()));
    return false;
  }

  // Ny anslutning = ny sync-status
  desiredProfileSeenThisConnect = false;

  g_mqttOk = true;
  g_lastNetFailReason = "NONE";

  logSystem("MQTT: connected OK");

  bool subDesiredProfile = mqttClient->subscribe(MQTT_TOPIC_DESIRED_PROFILE);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_DESIRED_PROFILE + " " +
            (subDesiredProfile ? "OK" : "FAILED"));

  bool subDownlink = mqttClient->subscribe(MQTT_TOPIC_DOWNLINK);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_DOWNLINK + " " +
            (subDownlink ? "OK" : "FAILED"));

  bool subCmdAck = mqttClient->subscribe(MQTT_TOPIC_CMD_ACK);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_CMD_ACK + " " +
            (subCmdAck ? "OK" : "FAILED"));

  bool subNetMode = mqttClient->subscribe(MQTT_TOPIC_NET_MODE_DESIRED);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_NET_MODE_DESIRED + " " +
            (subNetMode ? "OK" : "FAILED"));

  bool subsOk = subDesiredProfile && subDownlink && subCmdAck && subNetMode;

  if (!subsOk)
  {
    g_mqttOk = false;
    g_lastNetFailReason = "MQTT_SUBSCRIBE_FAILED";
    logSystem("MQTT: connect incomplete, one or more subscribes failed");
    mqttClient->disconnect();
    return false;
  }

  return true;
}

bool mqttPublishAlive()
{
  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish alive, not connected");
    return false;
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("ALIVE", true) + ",";
  payload += "\"uptime_s\":" + String(millis() / 1000);
  payload += "}";

  logSystem("MQTT: publishing alive to " + String(MQTT_TOPIC_ALIVE) + " payload=" + payload);
  logSystem("MQTT: alive payload bytes=" + String(payload.length()));

  bool ok = mqttClient->publish(MQTT_TOPIC_ALIVE, payload.c_str());

  if (!ok)
  {
    logSystem("MQTT: alive publish FAILED");
    return false;
  }

  logSystem("MQTT: alive published OK");
  return true;
}

bool mqttPublishHealth(uint32_t recoveryCountBoot,
                       const char *lastRecoveryReason,
                       uint32_t netConnectCountBoot,
                       uint32_t mqttConnectCountBoot,
                       uint32_t lastNetConnectMs,
                       bool pendingProfileAck,
                       bool pirPending)
{
  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish health, not connected");
    return false;
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("HEALTH", false) + ",";
  payload += "\"uptime_s\":" + String(millis() / 1000) + ",";
  payload += "\"recovery_count_boot\":" + String(recoveryCountBoot) + ",";
  payload += "\"last_recovery_reason\":\"" + String(lastRecoveryReason) + "\",";
  payload += "\"net_connect_count_boot\":" + String(netConnectCountBoot) + ",";
  payload += "\"mqtt_connect_count_boot\":" + String(mqttConnectCountBoot) + ",";
  payload += "\"last_net_connect_ms\":" + String(lastNetConnectMs) + ",";
  payload += "\"mqtt_connected\":true,";
  payload += "\"pending_profile_ack\":" + String(pendingProfileAck ? "true" : "false") + ",";
  payload += "\"pir_pending\":" + String(pirPending ? "true" : "false");
  payload += "}";

  logSystem("MQTT: publishing health to " + String(MQTT_TOPIC_HEALTH) + " payload=" + payload);
  logSystem("MQTT: health payload bytes=" + String(payload.length()));

  bool ok = mqttClient->publish(MQTT_TOPIC_HEALTH, payload.c_str());

  if (!ok)
  {
    logSystem("MQTT: health publish FAILED");
    return false;
  }

  logSystem("MQTT: health published OK");
  return true;
}

bool mqttPublishPirEvent(uint32_t eventId,
                         uint16_t count,
                         uint32_t firstMs,
                         uint32_t lastMs,
                         uint8_t srcMask)
{
  if (!mqttClient || !mqttClient->connected())
  {
    return false;
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("PIR", true) + ",";
  payload += "\"pir_event_id\":" + String(eventId) + ",";
  payload += "\"count\":" + String(count) + ",";
  payload += "\"first_ms\":" + String(firstMs) + ",";
  payload += "\"last_ms\":" + String(lastMs) + ",";
  payload += "\"src_mask\":" + String(srcMask);
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_PIR, payload.c_str(), false);

  logSystem(String("MQTT: PIR publish ") + (ok ? "OK" : "FAIL") +
            " topic=" + String(MQTT_TOPIC_PIR) +
            " event_id=" + String(eventId) +
            " src_mask=" + String(srcMask) +
            " count=" + String(count));

  return ok;
}


bool mqttPublishActivityEvent(const char *source,
                              bool boostActive,
                              uint32_t durationS,
                              uint32_t untilMs,
                              const char *detail)
{
  if (!mqttClient || !mqttClient->connected())
  {
    return false;
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("ACTIVITY", true) + ",";
  payload += "\"source\":\"" + String(source ? source : "UNKNOWN") + "\",";
  payload += "\"boost_active\":" + String(boostActive ? "true" : "false") + ",";
  payload += "\"boost_duration_s\":" + String(durationS) + ",";
  payload += "\"boost_until_ms\":" + String(untilMs) + ",";
  payload += "\"detail\":\"" + String(detail ? detail : "") + "\"";
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_ACTIVITY, payload.c_str(), false);

  logSystem(String("MQTT: activity publish ") + (ok ? "OK" : "FAIL") +
            " topic=" + String(MQTT_TOPIC_ACTIVITY) +
            " source=" + String(source ? source : "UNKNOWN") +
            " active=" + String(boostActive ? "true" : "false"));

  return ok;
}

bool mqttPublishGpsSingle(const ExtGnssFix &fx, bool fixOk)
{
  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish gps(single), not connected");
    return false;
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("GPS", true) + ",";
  payload += "\"mode\":\"single\",";
  payload += "\"fix_ok\":" + String(fixOk ? "true" : "false") + ",";
  payload += "\"valid\":" + String(fx.valid ? "true" : "false") + ",";
  payload += "\"fix_mode\":" + String(fx.fixMode) + ",";
  payload += "\"fix_quality\":" + String(fx.fixQuality) + ",";
  payload += "\"sats\":" + String(fx.sats) + ",";
  payload += "\"hdop\":" + String(fx.hdop, 1) + ",";

  if (fixOk)
  {
    payload += "\"lat\":" + String(fx.lat, 6) + ",";
    payload += "\"lon\":" + String(fx.lon, 6) + ",";
    payload += "\"speed_kmh\":" + String(fx.speedKmh, 1) + ",";
    payload += "\"alt_m\":" + String(fx.altM, 1);
  }
  else
  {
    payload += "\"speed_kmh\":0.0,";
    payload += "\"alt_m\":0.0";
  }

  payload += "}";

  logSystem("MQTT: publishing gps(single) to " + String(MQTT_TOPIC_GPS_SINGLE) + " payload=" + payload);
  logSystem("MQTT: gps(single) payload bytes=" + String(payload.length()));

  bool ok = mqttClient->publish(MQTT_TOPIC_GPS_SINGLE, payload.c_str());

  if (!ok)
  {
    logSystem("MQTT: gps(single) publish FAILED");
    return false;
  }

  logSystem("MQTT: gps(single) published OK");
  return true;
}

bool mqttPublishVictronStateIfPending()
{
  if (!victronManagerPublishPending())
  {
    return true;
  }

  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish Victron state, not connected");
    return false;
  }

  String payload = victronManagerBuildStateJson();
  bool ok = mqttClient->publish(MQTT_TOPIC_VICTRON_STATE, payload.c_str(), true);

  logSystem(String("MQTT: Victron state publish ") + (ok ? "OK" : "FAILED") +
            " topic=" + String(MQTT_TOPIC_VICTRON_STATE) +
            " bytes=" + String(payload.length()));

  if (ok)
  {
    victronManagerClearPublishPending();
  }

  return ok;
}

bool mqttPublishNetStatus()
{
  mqttLoadNetModeFromNvs();

  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish net status, not connected");
    return false;
  }

  // Uppdatera RSSI precis före publish om respektive länk är aktiv.
  if (g_activeLink == "WIFI" && WiFi.status() == WL_CONNECTED)
  {
    g_wifiRssi = WiFi.RSSI();
  }
  if (g_activeLink == "SIM")
  {
    g_modemRssi = modemGetSignalQuality();
  }

  String payload = "{";
  payload += mqttBuildCommonJsonFields("NET", true) + ",";
  payload += "\"active_link\":\"" + g_activeLink + "\",";
  payload += "\"net_mode\":\"" + g_desiredNetMode + "\",";
  payload += "\"net_mode_change_id\":" + String(g_netModeChangeId) + ",";
  payload += "\"change_id\":" + String(g_netModeChangeId) + ",";
  payload += "\"wifi_ok\":" + String(g_wifiOk ? "true" : "false") + ",";
  payload += "\"sim_ok\":" + String(g_simOk ? "true" : "false") + ",";
  payload += "\"mqtt_ok\":" + String(g_mqttOk ? "true" : "false") + ",";

  if (g_wifiOk || g_activeLink == "WIFI")
  {
    payload += "\"wifi_rssi\":" + String(g_wifiRssi) + ",";
  }
  else
  {
    payload += "\"wifi_rssi\":null,";
  }

  if (g_simOk || g_activeLink == "SIM")
  {
    payload += "\"modem_rssi\":" + String(g_modemRssi) + ",";
  }
  else
  {
    payload += "\"modem_rssi\":null,";
  }

  payload += "\"last_fail_reason\":\"" + g_lastNetFailReason + "\"";
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_NET_STATUS, payload.c_str(), false);

  logSystem(String("MQTT: net status publish ") + (ok ? "OK" : "FAILED") +
            " payload=" + payload);

  return ok;
}

const char *mqttGetDesiredNetMode()
{
  mqttLoadNetModeFromNvs();
  return g_desiredNetMode.c_str();
}

bool mqttNetModeSwitchRequested()
{
  return g_netModeSwitchRequested;
}

void mqttClearNetModeSwitchRequested()
{
  g_netModeSwitchRequested = false;
}

bool mqttLoop()
{
  if (!mqttClient)
  {
    return false;
  }

  if (!mqttClient->connected())
  {
    return false;
  }

  bool ok = mqttClient->loop();

  if (!ok && !mqttClient->connected())
  {
    logSystem("MQTT: loop detected disconnect");
    return false;
  }

  return mqttClient->connected();
}

void mqttDisconnect()
{
  if (mqttClient && mqttClient->connected())
  {
    logSystem("MQTT: disconnect");
    mqttClient->disconnect();
  }

  g_mqttOk = false;

  // Ny session får ny sync-status.
  desiredProfileSeenThisConnect = false;
}

bool mqttIsConnected()
{
  return mqttClient && mqttClient->connected();
}

bool mqttHasSeenDesiredProfileThisConnect()
{
  return desiredProfileSeenThisConnect;
}