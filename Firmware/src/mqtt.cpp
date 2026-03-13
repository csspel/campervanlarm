#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "ext_gnss.h"
#include "modem.h"
#include "profiles.h"
#include "time_manager.h"

#include <PubSubClient.h>

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
static uint32_t msgCounter = 0;
static uint32_t lastHandledProfileChangeId = 0;
static bool desiredProfileSeenThisConnect = false;

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
static void mqttPublishAck(uint32_t profileChangeId, const char *status, const char *detail = "")
{
  if (!mqttClient || !mqttClient->connected())
    return;

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

  // Applicera ny profil
  setProfile(pid);

  mqttPublishAck(profileChangeId,
                 "OK",
                 fromLegacyDownlink ? "profile_set_from_legacy" : "profile_set");

  // Publicera alive direkt så HA snabbt ser aktuell profil.
  mqttPublishAlive();
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
void mqttSetup()
{
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
    logSystem("MQTT: connect FAILED, rc=" + String(mqttClient->state()));
    return false;
  }

  // Ny anslutning = ny sync-status
  desiredProfileSeenThisConnect = false;

  logSystem("MQTT: connected OK");

  // Ny retained desired-state topic
  bool subDesiredProfile = mqttClient->subscribe(MQTT_TOPIC_DESIRED_PROFILE);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_DESIRED_PROFILE + " " +
            (subDesiredProfile ? "OK" : "FAILED"));

  // Legacy / framtida command topic
  bool subDownlink = mqttClient->subscribe(MQTT_TOPIC_DOWNLINK);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_DOWNLINK + " " +
            (subDownlink ? "OK" : "FAILED"));

  // PIR ACK
  bool subCmdAck = mqttClient->subscribe(MQTT_TOPIC_CMD_ACK);
  logSystem(String("MQTT: subscribe ") + MQTT_TOPIC_CMD_ACK + " " +
            (subCmdAck ? "OK" : "FAILED"));

  return subDesiredProfile && subDownlink && subCmdAck;
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

void mqttLoop()
{
  if (mqttClient && mqttClient->connected())
  {
    mqttClient->loop();
  }
}

void mqttDisconnect()
{
  if (mqttClient && mqttClient->connected())
  {
    logSystem("MQTT: disconnect");
    mqttClient->disconnect();
  }
}

bool mqttIsConnected()
{
  return mqttClient && mqttClient->connected();
}

bool mqttHasSeenDesiredProfileThisConnect()
{
  return desiredProfileSeenThisConnect;
}