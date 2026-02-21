# Campervanlarm – MQTT payload spec (v1.0)

Device: ellie
Topic base: van/ellie/
Transport: MQTT (LTE-M/NB-IoT)
Format: JSON (UTF-8)
Tid: epoch_utc i sekunder (UTC)

Det här dokumentet beskriver topics och payloads som ska gälla för firmware, Home Assistant och Node-RED.

====================================================================
0) ÖVERSIKT
====================================================================

TOPICS

1. van/ellie/cmd/downlink   (HA -> device)  retain=true  qos=1
   Syfte: Sätter önskat läge + framtida config/commands

2. van/ellie/ack            (device -> HA)  retain=false qos=0/1
   Syfte: Bekräftar att downlink applicerats

3. van/ellie/tele/alive     (device -> HA)  retain=false qos=0
   Syfte: Periodisk status

4. van/ellie/tele/gps       (device -> HA)  retain=false qos=0
   Syfte: GPS-data (single eller batch i samma topic)

5. van/ellie/tele/pir       (device -> HA)  retain=false qos=0
   Syfte: PIR-event (intrång)

6. van/ellie/cmd/ack        (HA -> device)  retain=false qos=0
   Syfte: ACK tillbaka för event (PIR_ACK)

LÄGEN (profiles)
- PARKED
- TRAVEL
- ARMED
- TRIGGERED

====================================================================
1) GENERELLA REGLER
====================================================================

Fältkonventioner:
- device_id (string) bör vara "ellie" i uplink-meddelanden.
- type (string) skiljer telemetrier: "ALIVE", "GPS", "PIR".
- profile (string) ska finnas i alive och gps.
- epoch_utc (int) används när tid är känd.
- lat/lon i grader (decimal), speed_kmh i km/h.

Robusthet:
- Device ska kunna skicka alive och pir även om GNSS saknar fix.
- GPS single ska kunna skickas även utan fix (UC-01 val A), då utan lat/lon.

Versionshantering (rekommenderat):
- Du kan lägga till schema_ver: "1.0" i alla payloads (inte hårt krav ännu).

====================================================================
2) DOWNLINK: van/ellie/cmd/downlink (retained)
====================================================================

Syfte:
HA talar om vilket läge enheten ska köra i och kan senare skicka config/commands.

Payload (JSON) – Exempel: sätt läge (ARMED)
{
  "ack_msg_id": 101,
  "desired_profile": "ARMED",
  "config": {},
  "commands": []
}

Fält:
- ack_msg_id (int, required)
  Monoton räknare från HA. Används för att matcha device-ACK.
- desired_profile (string, required)
  PARKED|TRAVEL|ARMED|TRIGGERED
- config (object, optional)
  Reserverat för framtida inställningar.
- commands (array, optional)
  Reserverat för framtida kommandon.

Retained-beteende:
- HA publicerar downlink med retain=true.
- Device applicerar läget och skickar van/ellie/ack.
- Efter OK-ACK rensar HA retained downlink genom att publicera tom payload med retain=true.

Exempel: rensa retained (tom sträng som payload)
""

====================================================================
3) DOWNLINK-ACK: van/ellie/ack (device -> HA)
====================================================================

Syfte:
Device bekräftar att den har applicerat senaste downlink.

Payload (JSON) – Exempel: OK
{
  "ack_msg_id": 101,
  "status": "OK",
  "profile": "ARMED",
  "detail": ""
}

Payload (JSON) – Exempel: ERR
{
  "ack_msg_id": 101,
  "status": "ERR",
  "profile": "PARKED",
  "detail": "unknown profile"
}

Fält:
- ack_msg_id (int, required)
  Ska matcha cmd/downlink.ack_msg_id
- status (string, required)
  OK eller ERR
- profile (string, required)
  Aktuellt läge efter applicering
- detail (string, optional)
  Feltext vid ERR

====================================================================
4) ALIVE: van/ellie/tele/alive (device -> HA)
====================================================================

Syfte:
Periodiskt hälsomeddelande.

Payload (JSON) – Exempel (rekommenderad)
{
  "device_id": "ellie",
  "type": "ALIVE",
  "msg_id": "12345",
  "profile": "PARKED",
  "epoch_utc": 1765914600,
  "uptime_s": 3600,
  "mqtt_ok": true,
  "gnss_ok": true,
  "fix_ok": false,
  "battery_v": 12.6,
  "moving": false
}

Minimikrav:
- type="ALIVE"
- profile
- uptime_s (starkt rekommenderat)

Kommentar:
- fix_ok kan vara false även om GNSS är igång (acquiring).

====================================================================
5) GPS: van/ellie/tele/gps (device -> HA)
====================================================================

Syfte:
GPS skickas i en topic. mode avgör om payloaden är single eller batch.

5.1 SINGLE (med fix) – Exempel
{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "single",
  "profile": "PARKED",
  "epoch_utc": 1765914535,
  "fix_ok": true,
  "lat": 57.27262,
  "lon": 10.421945,
  "speed_kmh": 0.0,
  "course_deg": 12.3
}

5.2 SINGLE (utan fix) – UC-01 val A
Skickas ändå men utan lat/lon:
{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "single",
  "profile": "PARKED",
  "epoch_utc": 1765914535,
  "fix_ok": false
}

5.3 BATCH (TRAVEL) – Exempel
Policy:
- var 5 min
- 30 punkter à 10 s
Lagring:
- RAM ringbuffer tills sändning lyckas (försvinner vid reboot)

{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "batch",
  "profile": "TRAVEL",
  "t0": 1765914000,
  "dt": 10,
  "fixes": [
    [57.27250, 10.42180, 0, 0, 0.0],
    [57.27251, 10.42181, 0, 0, 0.1],
    [57.27260, 10.42190, 0, 0, 12.4]
  ]
}

Fält:
- mode (string, required): single|batch
- epoch_utc (int): för single
- t0 (int): epoch för första batchpunkten
- dt (int): tidsteg i sekunder mellan punkter
- fixes (array): lista med punkter

fixes-format (nuvarande HA-template förväntar):
- fx[0] = lat
- fx[1] = lon
- fx[4] = speed_kmh
Index 2–3 reserverade för framtiden (t.ex. alt/course).

====================================================================
6) PIR EVENT: van/ellie/tele/pir (device -> HA)
====================================================================

Syfte:
Intrångshändelse från PIR (endast i ARMED).
Throttle:
max 1 event/minut per PIR till MQTT.

Payload (JSON) – Exempel
{
  "device_id": "ellie",
  "type": "PIR",
  "pir": 1,
  "pir_event_id": 987,
  "count": 4,
  "profile": "ARMED",
  "epoch_utc": 1765914600
}

Fält:
- pir (int, required): 1 eller 2
- pir_event_id (int, required): monoton räknare per device (för ack)
- count (int, optional): intern räknare
- profile (string, required): bör vara ARMED
- epoch_utc (int, recommended)

====================================================================
7) EVENT-ACK: van/ellie/cmd/ack (HA -> device)
====================================================================

Syfte:
HA bekräftar mottaget PIR-event så device kan markera det som hanterat.

Payload (JSON) – PIR_ACK Exempel
{
  "type": "PIR_ACK",
  "pir_event_id": 987
}

Fält:
- type (string, required): PIR_ACK
- pir_event_id (int, required): matchar eventet

====================================================================
8) INTERVALL (från krav v5)
====================================================================

- PARKED:
  alive var 5 min + GPS single vid varje alive (även utan fix)
- TRAVEL:
  batch var 5 min (30 punkter à 10 s) + alive var 5 min
- TRIGGERED:
  GPS single var 15 s + alive samtidigt, MQTT hålls uppe
- ARMED:
  alive var 30 min och annars sleep
- ARMED_AWAKE (sub-läge efter PIR):
  alive var 2 min, sliding window 30 min, max 2 h

====================================================================
9) KOMPATIBILITET MED HOME ASSISTANT
====================================================================

- HA device_tracker läser van/ellie/tele/gps
- Single utan fix saknar lat/lon -> HA-template sätter då inte latitude/longitude
  (hanteras i home-assistant/packages/van.yaml)
