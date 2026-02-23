#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "gps.h"
#include "modem.h"
#include "profiles.h"
#include "time_manager.h"

#include <PubSubClient.h>

#ifndef MQTT_TOPIC_VERSION
#define MQTT_TOPIC_VERSION "van/ellie/tele/version"
#endif

static Client *netClient = nullptr;
static PubSubClient *mqttClient = nullptr;
static uint32_t msgCounter = 0;

extern void pipelineOnPirAck(uint32_t eventId);

// Downlink state
static String lastDownlinkRaw;
static uint32_t lastAckMsgId = 0; // dedupe på ack_msg_id

// ----------------- Minimal JSON helpers (som du redan hade) -----------------
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

// ----------------- Internal helpers -----------------
static void clearRetainedDownlink()
{
  if (!mqttClient || !mqttClient->connected())
    return;

  // Tom retained payload rensar retained msg på broker (Mosquitto/HA funkar så)
  bool ok = mqttClient->publish(MQTT_TOPIC_DOWNLINK, "", true);
  logSystem(String("MQTT: clear retained downlink ") + (ok ? "OK" : "FAILED"));
}

static void mqttPublishAck(uint32_t ackMsgId, const char *status, const char *detail = "")
{
  if (!mqttClient || !mqttClient->connected())
    return;

  const ProfileConfig &p = currentProfile();

  // {"device_id":"...","type":"ACK","ack_msg_id":1234,"status":"OK","detail":"...","profile":"ARMED","fw":"...","epoch_utc":...}
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"type\":\"ACK\",";
  payload += "\"ack_msg_id\":" + String(ackMsgId) + ",";
  payload += "\"status\":\"" + String(status) + "\",";
  payload += "\"detail\":\"" + String(detail) + "\",";
  payload += "\"profile\":\"" + String(p.name) + "\",";

#ifdef FW_VERSION
  payload += "\"fw\":\"" + String(FW_VERSION) + "\",";
#endif

  payload += "\"epoch_utc\":" + String(timeEpochUtc());
  payload += "}";

  mqttClient->publish(MQTT_TOPIC_ACK, payload.c_str(), false);
  logSystem("MQTT: ACK published payload=" + payload);
}

// ----------------- Robust downlink callback -----------------
static void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  String t(topic);
  String msg;

  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  // Ignore retained clear (empty payload)
  msg.trim();
  if (msg.length() == 0)
  {
    return;
  }

  logSystem("MQTT: RX topic=" + t + " payload=" + msg);
  lastDownlinkRaw = msg;

  // Server-ACK för PIR-event (Risk 1 steg 2)
  if (t == MQTT_TOPIC_CMD_ACK)
  {
    // Exempel: {"type":"PIR_ACK","pir_event_id":123}
    String typ = jsonGetString(msg, "type");
    uint32_t eid = jsonGetUInt(msg, "pir_event_id");
    if (eid == 0)
      eid = jsonGetUInt(msg, "event_id"); // tolerant

    if ((typ.length() == 0 || typ == "PIR_ACK") && eid != 0)
    {
      pipelineOnPirAck(eid);
      logSystem("MQTT: PIR_ACK received event_id=" + String(eid));
    }
    return;
  }

  if (t != MQTT_TOPIC_DOWNLINK)
    return;

  // Robustness:
  // - kräver ack_msg_id
  // - dedupe så retained inte körs om igen
  // - clear retained efter hantering
  uint32_t ackId = jsonGetUInt(msg, "ack_msg_id");
  String desired = jsonGetString(msg, "desired_profile");

  if (ackId == 0)
  {
    mqttPublishAck(0, "ERROR", "missing_ack_msg_id");
    // rensa INTE retained här, eftersom vi inte vet vad avsändaren vill (men du kan välja att rensa även här)
    return;
  }

  // Dedupe: om broker spelar upp retained igen efter reconnect → ignorera
  if (ackId == lastAckMsgId)
  {
    mqttPublishAck(ackId, "DUPLICATE_IGNORED", "same_ack_msg_id");
    // clearRetainedDownlink();
    return;
  }
  lastAckMsgId = ackId;

  if (desired.length() > 0)
  {
    ProfileId pid;
    if (profileFromString(desired, pid))
    {
      setProfile(pid);
      mqttPublishAck(ackId, "OK", "profile_set");
      mqttPublishAlive(); // direkt feedback
    }
    else
    {
      mqttPublishAck(ackId, "ERROR", "unknown_profile");
    }
  }
  else
  {
    mqttPublishAck(ackId, "OK", "no_profile_change");
  }

  // Viktigt: rensa retained så den inte triggar igen vid nästa connect
  // clearRetainedDownlink();
}

// ----------------- Public API -----------------
void mqttSetup()
{
  if (!netClient)
  {
    netClient = &modemGetClient();
  }
  if (!mqttClient)
  {
    mqttClient = new PubSubClient(*netClient);
    mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient->setCallback(mqttCallback);

    mqttClient->setBufferSize(2048); // öka buffer för större payloads framförallt för batch GPS
    mqttClient->setKeepAlive(30);
    mqttClient->setSocketTimeout(15);
  }
}

bool mqttConnect()
{
  if (!mqttClient)
    mqttSetup();

  logSystem("MQTT: connecting to broker");
  mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  logSystem(String("MQTT: host=") + MQTT_BROKER_HOST + ":" + String(MQTT_BROKER_PORT));

  bool ok;
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

  logSystem("MQTT: connected OK");

  mqttClient->subscribe(MQTT_TOPIC_DOWNLINK);
  logSystem("MQTT: subscribed " + String(MQTT_TOPIC_DOWNLINK));

  mqttClient->subscribe(MQTT_TOPIC_CMD_ACK);
  logSystem("MQTT: subscribed " + String(MQTT_TOPIC_CMD_ACK));

  // Publicera version vid varje connect (retain så HA alltid vet vad som kör)
  // mqttPublishVersion(true);

  return true;
}

bool mqttPublishVersion(bool retain)
{
  if (!mqttClient || !mqttClient->connected())
    return false;

  // Håll payload kort; du kan alltid lägga till mer senare
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";

#ifdef FW_VERSION
  payload += "\"fw\":\"" + String(FW_VERSION) + "\",";
#else
  payload += "\"fw\":\"unknown\",";
#endif

  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeIsValid() ? "true" : "false") + ",";
  payload += "\"time_source\":\"" +
             String((timeGetSource() == TimeSource::MODEM) ? "MODEM" : (timeGetSource() == TimeSource::NTP) ? "NTP"
                                                                                                            : "NONE") +
             "\",";
  payload += "\"date_local\":\"" + timeDateLocal() + "\",";
  payload += "\"time_local\":\"" + timeClockLocal() + "\",";
  payload += "\"profile\":\"" + String(currentProfile().name) + "\"";
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_VERSION, payload.c_str(), retain);
  logSystem(String("MQTT: publish version ") + (ok ? "OK" : "FAILED") + " payload=" + payload);
  return ok;
}

bool mqttPublishAlive()
{
  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish alive, not connected");
    return false;
  }

  uint32_t upSeconds = millis() / 1000;
  const ProfileConfig &p = currentProfile();

  msgCounter++;
  String msgId = String(msgCounter);

  const bool timeValid = timeIsValid();
  const String isoUtc = timeIsoUtc();

  const char *src = "NONE";
  if (timeGetSource() == TimeSource::MODEM)
    src = "MODEM";
  else if (timeGetSource() == TimeSource::NTP)
    src = "NTP";

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"msg_id\":\"" + msgId + "\",";
  payload += "\"type\":\"ALIVE\",";
  payload += "\"timestamp\":\"" + isoUtc + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeValid ? "true" : "false") + ",";
  payload += "\"time_source\":\"" + String(src) + "\",";
  payload += "\"date_local\":\"" + timeDateLocal() + "\",";
  payload += "\"time_local\":\"" + timeClockLocal() + "\",";
  payload += "\"profile\":\"" + String(p.name) + "\",";
  payload += "\"uptime_s\":" + String(upSeconds);
  payload += "}";

  logSystem("MQTT: publishing alive to " + String(MQTT_TOPIC_ALIVE) + " payload=" + payload);
  logSystem("MQTT: alive payload bytes=" + String(payload.length()));

  bool ok = mqttClient->publish(MQTT_TOPIC_ALIVE, payload.c_str());
  if (!ok)
  {
    logSystem("MQTT: publish FAILED");
    return false;
  }

  logSystem("MQTT: alive published OK, msg_id=" + msgId);
  return true;
}

bool mqttPublishPirEvent(uint32_t eventId, uint16_t count, uint32_t firstMs, uint32_t lastMs, uint8_t srcMask)

{
  if (!mqttClient || !mqttClient->connected())
    return false;

  const ProfileConfig &p = currentProfile();

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"msg_id\":\"" + String(++msgCounter) + "\",";
  payload += "\"type\":\"PIR\",";
  payload += "\"pir_event_id\":" + String(eventId) + ",";
  payload += "\"count\":" + String(count) + ",";
  payload += "\"first_ms\":" + String(firstMs) + ",";
  payload += "\"last_ms\":" + String(lastMs) + ",";
  payload += "\"src_mask\":" + String(srcMask) + ",";
  payload += "\"profile\":\"" + String(p.name) + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc());
  payload += "}";

  bool ok = mqttClient->publish(MQTT_TOPIC_PIR, payload.c_str(), false);
  logSystem(String("MQTT: PIR publish ") + (ok ? "OK" : "FAIL") +
            " topic=" + String(MQTT_TOPIC_PIR) +
            " event_id=" + String(eventId) +
            " src_mask=" + String(srcMask) +
            " count=" + String(count));
  return ok;
}

bool mqttPublishGpsSingle(const GpsFix &fx, bool fixOk)
{
  if (!mqttClient || !mqttClient->connected())
  {
    logSystem("MQTT: cannot publish gps(single), not connected");
    return false;
  }

  msgCounter++;
  String msgId = String(msgCounter);

  const char *src = "NONE";
  if (timeGetSource() == TimeSource::MODEM)
    src = "MODEM";
  else if (timeGetSource() == TimeSource::NTP)
    src = "NTP";

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"msg_id\":\"" + msgId + "\",";
  payload += "\"type\":\"GPS\",";
  payload += "\"mode\":\"single\",";
  payload += "\"timestamp\":\"" + timeIsoUtc() + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeIsValid() ? "true" : "false") + ",";
  payload += "\"time_source\":\"" + String(src) + "\",";
  payload += "\"date_local\":\"" + timeDateLocal() + "\",";
  payload += "\"time_local\":\"" + timeClockLocal() + "\",";
  payload += "\"profile\":\"" + String(currentProfile().name) + "\",";
  payload += "\"fix_ok\":" + String(fixOk ? "true" : "false") + ",";
  payload += "\"start_mode\":\"" + String((fx.start_mode == 3) ? "HOT" : (fx.start_mode == 2) ? "WARM"
                                                                     : (fx.start_mode == 1)   ? "COLD"
                                                                                              : "UNKNOWN") +
             "\",";
  payload += "\"ttff_s\":" + String(fx.ttff_s) + ",";
  payload += "\"valid\":" + String(fx.valid ? "true" : "false") + ",";
  payload += "\"fix_age_ms\":" + String(fx.fix_age_ms) + ",";
  payload += "\"fix_mode\":" + String(fx.fix_mode) + ",";

  // Positionfält (om valid, annars ändå 0.0 så Node-RED kan logga)
  if (fixOk)
  {
    payload += "\"lat\":" + String(fx.lat, 6) + ",";
    payload += "\"lon\":" + String(fx.lon, 6) + ",";
    payload += "\"speed_kmh\":" + String(fx.speed_kmh, 1) + ",";
    payload += "\"course_deg\":" + String(fx.course_deg, 1) + ",";
    payload += "\"alt_m\":" + String(fx.alt_m, 1);
  }
  else
  {
    // UC-01 val A: inga lat/lon när fix saknas
    payload += "\"speed_kmh\":0.0,";
    payload += "\"course_deg\":0.0,";
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

  logSystem("MQTT: gps(single) published OK, msg_id=" + msgId);
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

// void mqttLoopFor(uint32_t durationMs)
//{
//   if (!mqttClient || !mqttClient->connected())
//     return;
//   uint32_t start = millis();
//   while (millis() - start < durationMs)
//   {
//     mqttClient->loop();
//     delay(10);
//   }
// }
