
# Campervanlarm – MQTT payload spec (v1.0)

**Device:** `ellie`  
**Topic base:** `van/ellie/`  
**Transport:** MQTT (via LTE-M/NB-IoT)  
**Format:** JSON (UTF-8)  
**Tid:** `epoch_utc` i sekunder (UTC)

Det här dokumentet beskriver topics och payloads som ska gälla för firmware, Home Assistant och Node-RED.

---

## 0) Översikt

| Topic | Riktning | Retain | QoS | Syfte |
|---|---|---:|---:|---|
| `van/ellie/cmd/downlink` | HA → device | **true** | **1** | Sätter önskat läge + framtida config/commands |
| `van/ellie/ack` | device → HA | false | 0/1 | Bekräftar att downlink applicerats |
| `van/ellie/tele/alive` | device → HA | false | 0 | Periodisk status |
| `van/ellie/tele/gps` | device → HA | false | 0 | GPS data (single eller batch i samma topic) |
| `van/ellie/tele/pir` | device → HA | false | 0 | PIR-event (intrång) |
| `van/ellie/cmd/ack` | HA → device | false | 0 | Ack tillbaka för event (PIR_ACK) |

### Lägen (profiles)
`PARKED`, `TRAVEL`, `ARMED`, `TRIGGERED`

---

## 1) Generella regler

### 1.1 Fältkonventioner
- `device_id` (string) bör vara `"ellie"` i uplink-meddelanden.
- `type` (string) används för att skilja olika telemetrier: `"ALIVE"`, `"GPS"`, `"PIR"`.
- `profile` (string) ska finnas i `alive` och `gps`.
- `epoch_utc` (int) används när tid är känd.
- Numeriska värden: `lat/lon` i grader (decimal), `speed_kmh` i km/h.

### 1.2 Robusthet
- Device ska kunna skicka `alive` och `pir` även om GNSS saknar fix.
- GPS `single` ska kunna skickas även utan fix (UC-01 val A), då utan lat/lon.

### 1.3 Versionshantering (rekommenderat)
Du kan lägga till t.ex. `schema_ver: "1.0"` i alla payloads. Inte ett hårt krav än.

---

## 2) Downlink: `van/ellie/cmd/downlink` (retained)

**Syfte:** HA talar om vilket läge enheten ska köra i och kan senare skicka config/commands.

### 2.1 Exempel: Sätt läge (ARMED)
```json
{
  "ack_msg_id": 101,
  "desired_profile": "ARMED",
  "config": {},
  "commands": []
}

2.2 Fält

ack_msg_id (int, required)
Monoton räknare från HA. Används för att matcha device-ACK.

desired_profile (string, required)
PARKED|TRAVEL|ARMED|TRIGGERED

config (object, optional)
Reserverat för framtida inställningar (intervall mm).

commands (array, optional)
Reserverat för framtida kommandon.

2.3 Retained-beteende

HA publicerar downlink med retain: true.

När device har applicerat läget skickar den van/ellie/ack.

Efter OK-ACK rensar HA retained downlink genom att publicera tom payload med retain:true.

Exempel: rensa retained

3) Downlink-ACK: van/ellie/ack (device → HA)

Syfte: Device bekräftar att den har applicerat senaste downlink.

3.1 Exempel: OK
{
  "ack_msg_id": 101,
  "status": "OK",
  "profile": "ARMED",
  "detail": ""
}
3.2 Exempel: ERR
{
  "ack_msg_id": 101,
  "status": "ERR",
  "profile": "PARKED",
  "detail": "unknown profile"
}
3.3 Fält

ack_msg_id (int, required) – ska matcha senaste cmd/downlink.ack_msg_id

status (string, required) – OK eller ERR

profile (string, required) – aktuellt läge efter applicering

detail (string, optional) – feltext vid ERR

4) Alive: van/ellie/tele/alive (device → HA)

Syfte: Periodiskt hälsomeddelande.

4.1 Exempel (rekommenderad payload)
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
4.2 Minimikrav

type = "ALIVE"

profile

uptime_s (starkt rekommenderat)

4.3 Kommentar

fix_ok kan vara false även om GNSS är igång (acquiring).

moving kan beräknas från speed eller annan logik (valfritt).

5) GPS: van/ellie/tele/gps (device → HA)

Syfte: Position skickas i en topic. mode avgör om payloaden är single eller batch.

5.1 Single (med fix)
{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "single",
  "profile": "PARKED",
  "epoch_utc": 1765914535,
  "fix_ok": true,
  "lat": 58.27262,
  "lon": 11.421945,
  "speed_kmh": 0.0,
  "course_deg": 12.3
}
5.2 Single (utan fix) – UC-01 val A

Skickas ändå, men utan lat/lon:

{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "single",
  "profile": "PARKED",
  "epoch_utc": 1765914535,
  "fix_ok": false
}
5.3 Batch (TRAVEL)

Policy (krav): var 5 min, 30 punkter à 10 s.
Lagring: RAM ringbuffer tills sändning lyckas (försvinner vid reboot).

{
  "device_id": "ellie",
  "type": "GPS",
  "mode": "batch",
  "profile": "TRAVEL",
  "t0": 1765914000,
  "dt": 10,
  "fixes": [
    [58.27250, 11.42180, 0, 0, 0.0],
    [58.27251, 11.42181, 0, 0, 0.1],
    [58.27260, 11.42190, 0, 0, 12.4]
  ]
}
5.4 Fält

mode (string, required): single eller batch

epoch_utc (int): för single

t0 (int): epoch för första batchpunkten

dt (int): tidsteg i sekunder mellan punkter

fixes (array): lista med punkter

fixes-format (nuvarande HA-template förväntar):

fx[0]=lat, fx[1]=lon, fx[4]=speed_kmh

Index 2–3 reserverade för framtiden (t.ex. alt/course)

6) PIR event: van/ellie/tele/pir (device → HA)

Syfte: Intrångshändelse från PIR.
Aktivt läge: endast i ARMED.
Throttle: max 1 event/minut per PIR till MQTT (men du kan serial-logga alla i utveckling).

6.1 Exempel
{
  "device_id": "ellie",
  "type": "PIR",
  "pir": 1,
  "pir_event_id": 987,
  "count": 4,
  "profile": "ARMED",
  "epoch_utc": 1765914600
}
6.2 Fält

pir (int, required): 1 eller 2 (vilken sensor)

pir_event_id (int, required): monoton räknare per device (för ack)

count (int, optional): intern räknare

profile (string, required): bör vara ARMED

epoch_utc (int, recommended)

7) Event-ACK: van/ellie/cmd/ack (HA → device)

Syfte: HA bekräftar mottaget PIR-event så device kan markera det som hanterat.

7.1 PIR_ACK exempel
{
  "type": "PIR_ACK",
  "pir_event_id": 987
}
7.2 Fält

type (string, required): PIR_ACK

pir_event_id (int, required): matchar eventet

8) Rekommenderade intervall per läge (från krav v5)

PARKED: alive var 5 min + GPS single vid varje alive (även utan fix, val A)

TRAVEL: batch var 5 min (30 punkter à 10 s) + alive var 5 min

TRIGGERED: GPS single var 15 s + alive samtidigt, MQTT hålls uppe

ARMED: alive var 30 min och annars sleep

ARMED_AWAKE (sub-läge efter PIR): alive var 2 min, sliding window 30 min, max 2 h

9) Kompatibilitet med Home Assistant van.yaml

HA device_tracker läser van/ellie/tele/gps

Single utan fix saknar lat/lon → HA-template ska då inte sätta latitude/longitude (hanteras i senaste van.yaml)
