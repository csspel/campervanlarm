# Campervanlarm – MQTT payload spec

**Version:** 2.0 draft  
**Device:** `ellie`  
**Topic base:** `van/ellie/`  
**Transport:** MQTT över LTE-M/NB-IoT  
**Format:** JSON (UTF-8)

Det här dokumentet beskriver rekommenderade MQTT-topics och payloads för **firmware**, **Home Assistant** och **Node-RED** i campervanlarm-projektet.

Målet är att ha en praktiskt användbar spec som:
- speglar nuvarande firmware bättre än tidigare version,
- tydligt skiljer på **aktivt format** och **legacy/migration**,
- gör det enkelt att hålla Home Assistant, Node-RED och firmware synkade.

---

## 0) Översikt

### Aktiva topics

1. `van/ellie/state/desired_profile`  
   **Riktning:** HA/Node-RED -> device  
   **retain:** `true`  
   **qos:** `1` rekommenderat  
   **Syfte:** Primär topic för önskad profil/state.

2. `van/ellie/ack`  
   **Riktning:** device -> HA/Node-RED  
   **retain:** `false`  
   **qos:** `0` eller `1`  
   **Syfte:** Bekräftelse på profiländring eller fel.

3. `van/ellie/tele/alive`  
   **Riktning:** device -> HA/Node-RED  
   **retain:** `false`  
   **qos:** `0`  
   **Syfte:** Periodisk status/hälsomeddelande.

4. `van/ellie/tele/gps`  
   **Riktning:** device -> HA/Node-RED  
   **retain:** `false`  
   **qos:** `0`  
   **Syfte:** GNSS-position och relaterad information.

5. `van/ellie/tele/pir`  
   **Riktning:** device -> HA/Node-RED  
   **retain:** `false`  
   **qos:** `0`  
   **Syfte:** PIR-händelser/intrångsindikering.

6. `van/ellie/cmd/ack`  
   **Riktning:** HA/Node-RED -> device  
   **retain:** `false`  
   **qos:** `0`  
   **Syfte:** Event-ACK tillbaka till device, idag främst `PIR_ACK`.

7. `van/ellie/tele/version`  
   **Riktning:** device -> HA/Node-RED  
   **retain:** valfritt  
   **qos:** `0`  
   **Syfte:** Version/build-information.

### Legacy / migration

8. `van/ellie/cmd/downlink`  
   **Riktning:** HA/Node-RED -> device  
   **retain:** normalt `false`  
   **Syfte:** Legacy-topic. Under migration tolereras fortfarande `desired_profile` här, men på sikt bör topicen användas för rena engångskommandon eller fasas ut för profilstyrning.

### Profiler / lägen

- `PARKED`
- `TRAVEL`
- `ARMED`
- `ARMED_AWAKE` *(internt/sub-läge, normalt ej satt direkt som external desired profile om du vill hålla integrationen enklare)*
- `TRIGGERED` *(om använd i pipeline/logik)*

> Rekommendation: dokumentera bara de profiler som faktiskt ska kunna sättas utifrån. Interna eller temporära pipeline-lägen bör beskrivas separat i systemdokumentationen.

---

## 1) Generella regler

### 1.1 Basfält

Följande fält bör användas konsekvent i uplink/telemetri när det är relevant:

- `device_id` (string) – normalt `"ellie"`
- `type` (string) – t.ex. `"ALIVE"`, `"GPS"`, `"PIR"`, `"ACK"`
- `profile` (string) – aktuell aktiv profil i device
- `timestamp` (string) – ISO-8601 UTC, t.ex. `"2026-03-08T16:55:27Z"`
- `epoch_utc` (int) – Unix-tid i sekunder
- `time_valid` (bool) – om tiden är giltig
- `time_source` (string) – t.ex. `"MODEM"`, `"NTP"`, `"NONE"`
- `date_local` (string) – lokal datumrepresentation
- `time_local` (string) – lokal tidsrepresentation

### 1.2 `msg_id`

För telemetri som skickas från device kan `msg_id` användas som löpnummer för publicerade meddelanden.

- `msg_id` är bra för felsökning och loggkorrelation.
- `msg_id` ersätter **inte** `profile_change_id` eller `pir_event_id` för semantisk kvittens.

### 1.3 JSON-format

- UTF-8
- Booleska värden ska skickas som JSON-bool (`true`/`false`), inte som sträng.
- Numeriska värden ska skickas som tal, inte som sträng, om det inte finns ett mycket starkt kompatibilitetsskäl.

### 1.4 Robusthet

- Device ska kunna skicka `ALIVE` även om GNSS saknar fix.
- Device ska kunna skicka `PIR` även om GNSS saknar fix.
- `GPS` får skickas även när `fix_ok=false`, men då utan `lat`/`lon` om ingen giltig position finns.

### 1.5 Rekommenderad versionshantering

Rekommenderade tillägg:
- `schema_ver`: t.ex. `"2.0"`
- `fw`: firmwareversion när relevant

De är inte nödvändiga i varje payload men rekommenderas i `ACK`, `ALIVE` och ev. `tele/version`.

---

## 2) Desired profile – `van/ellie/state/desired_profile`

**Status:** aktiv primär lösning  
**retain:** `true`

### Syfte

Home Assistant eller Node-RED publicerar den profil som device ska gå till.

### Rekommenderad payload

```json
{
  "profile_change_id": 101,
  "desired_profile": "ARMED",
  "source": "home_assistant",
  "reason": "user_selected_mode"
}
```

### Fält

- `profile_change_id` (int, required)  
  Monoton räknare eller annat unikt ID för just denna profiländring.

- `desired_profile` (string, required)  
  Profilen som device ska gå till.

- `source` (string, optional)  
  Vem som satte state, t.ex. `home_assistant`, `node_red`, `manual`, `automation`.

- `reason` (string, optional)  
  Felsökning/spårbarhet.

### Beteende

- Topicen publiceras med `retain=true`.
- Device läser retained state vid uppkoppling.
- Device skickar `van/ellie/ack` som bekräftelse.
- Retained state **ska normalt inte rensas efter ACK** om topicen används som faktisk desired state. Det är själva poängen att state ska ligga kvar.

> Detta är den största principiella skillnaden mot äldre `cmd/downlink`-modell. En state-topic ska normalt representera önskat tillstånd över tid, inte ett engångskommando som måste städas bort direkt.

---

## 3) Legacy downlink – `van/ellie/cmd/downlink`

**Status:** legacy / migration

### Syfte

Under migration tolererar device fortfarande `desired_profile` här.
På sikt bör denna topic reserveras för verkliga kommandon som inte är state, till exempel:
- begär en engångspublicering,
- trigga diagnostik,
- starta om modem,
- be om versionspayload.

### Legacy payload – fortfarande tolerant

```json
{
  "ack_msg_id": 101,
  "desired_profile": "ARMED"
}
```

### Kommentar

Nuvarande firmware verkar acceptera både:
- `profile_change_id` (nytt namn)
- `ack_msg_id` (fallback under migration)

### Rekommendation

- Ny implementation ska använda `van/ellie/state/desired_profile`.
- `cmd/downlink` bör markeras som legacy i HA/Node-RED-flöden.

---

## 4) ACK – `van/ellie/ack`

**Status:** aktiv

### Syfte

Bekräfta att device har tagit emot och hanterat profiländring.

### Rekommenderad payload

```json
{
  "device_id": "ellie",
  "type": "ACK",
  "profile_change_id": 101,
  "ack_msg_id": 101,
  "status": "OK",
  "detail": "profile_set",
  "profile": "ARMED",
  "fw": "2.2.0-dev",
  "epoch_utc": 1772988927
}
```

### Fält

- `device_id` (string, required)
- `type` = `"ACK"` (required)
- `profile_change_id` (int, required)
- `ack_msg_id` (int, optional men rekommenderat under migration)  
  Skickas med samma värde som `profile_change_id` för bakåtkompatibilitet.
- `status` (string, required)
- `detail` (string, optional)
- `profile` (string, required) – profil efter hantering
- `fw` (string, optional)
- `epoch_utc` (int, recommended)

### Rekommenderade statusvärden

- `OK`
- `ERROR`
- `DUPLICATE_IGNORED`

### Rekommenderade `detail`

- `profile_set`
- `profile_already_set`
- `profile_set_from_legacy`
- `profile_already_set_legacy`
- `missing_profile_change_id`
- `missing_desired_profile`
- `unknown_profile`

### Exempel: felaktig profil

```json
{
  "device_id": "ellie",
  "type": "ACK",
  "profile_change_id": 102,
  "ack_msg_id": 102,
  "status": "ERROR",
  "detail": "unknown_profile",
  "profile": "PARKED",
  "epoch_utc": 1772989000
}
```

---

## 5) ALIVE – `van/ellie/tele/alive`

**Status:** aktiv

### Syfte

Periodiskt hälsomeddelande från device.

### Rekommenderad payload

```json
{
  "device_id": "ellie",
  "msg_id": "118",
  "type": "ALIVE",
  "timestamp": "2026-03-08T16:55:27Z",
  "epoch_utc": 1772988927,
  "time_valid": true,
  "time_source": "NTP",
  "date_local": "2026-03-08",
  "time_local": "17:55:27",
  "profile": "TRAVEL",
  "uptime_s": 3600,
  "mqtt_ok": true,
  "fix_ok": true,
  "battery_v": 12.7,
  "moving": false
}
```

### Minimikrav

- `type = "ALIVE"`
- `profile`
- `timestamp` eller `epoch_utc`
- gärna `uptime_s`

### Kommentar

`fix_ok=false` betyder inte nödvändigtvis fel i GNSS-modulen. Det kan bara betyda att fix ännu inte finns.

---

## 6) GPS – `van/ellie/tele/gps`

**Status:** aktiv

### Syfte

Skicka GNSS-information. Nuvarande rekommendation är att huvudformatet är **single**.
`batch` kan finnas kvar som legacy eller framtida specialfall.

### 6.1 SINGLE – med fix

```json
{
  "device_id": "ellie",
  "msg_id": "119",
  "type": "GPS",
  "timestamp": "2026-03-08T16:55:27Z",
  "epoch_utc": 1772988927,
  "time_valid": true,
  "time_source": "NTP",
  "date_local": "2026-03-08",
  "time_local": "17:55:27",
  "profile": "TRAVEL",
  "mode": "single",
  "fix_ok": true,
  "valid": true,
  "fix_mode": 3,
  "fix_quality": 1,
  "sats": 12,
  "hdop": 0.7,
  "lat": 58.272552,
  "lon": 11.421799,
  "speed_kmh": 0.1,
  "alt_m": 23.9
}
```

### 6.2 SINGLE – utan fix

```json
{
  "device_id": "ellie",
  "msg_id": "120",
  "type": "GPS",
  "timestamp": "2026-03-08T16:56:00Z",
  "epoch_utc": 1772988960,
  "time_valid": true,
  "time_source": "NTP",
  "date_local": "2026-03-08",
  "time_local": "17:56:00",
  "profile": "PARKED",
  "mode": "single",
  "fix_ok": false,
  "valid": false,
  "fix_mode": 1,
  "sats": 0,
  "hdop": 99.9
}
```

### Fält

- `mode` (string, required): `single`
- `fix_ok` (bool, required)
- `valid` (bool, recommended)
- `fix_mode` (int, optional men bra)
- `fix_quality` (int, optional)
- `sats` (int, optional)
- `hdop` (number, optional)
- `lat` (number, only when valid fix)
- `lon` (number, only when valid fix)
- `speed_kmh` (number, optional)
- `alt_m` (number, optional)

### 6.3 BATCH – legacy / reserverad

Om batch används i framtiden eller i äldre logik:

```json
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
```

### Rekommendation

- Behandla `single` som huvudspec.
- Behåll `batch` som tydligt markerad legacy/reserverad del tills firmware och HA verkligen kräver den.

---

## 7) PIR event – `van/ellie/tele/pir`

**Status:** aktiv

### Syfte

Skicka intrångs- eller rörelsehändelser från PIR-sensorer.

### Rekommenderad payload (ny stil)

```json
{
  "device_id": "ellie",
  "msg_id": "121",
  "type": "PIR",
  "timestamp": "2026-03-08T16:57:10Z",
  "epoch_utc": 1772989030,
  "time_valid": true,
  "time_source": "NTP",
  "date_local": "2026-03-08",
  "time_local": "17:57:10",
  "profile": "ARMED",
  "pir_event_id": 987,
  "src_mask": 1,
  "count": 4
}
```

### Tolkning av `src_mask`

Exempel:
- `1` = framre PIR
- `2` = bakre PIR
- `3` = båda

Det gör payloaden mer framtidssäker än `pir: 1|2`.

### Legacy payload – äldre stil

```json
{
  "device_id": "ellie",
  "type": "PIR",
  "pir": 1,
  "pir_event_id": 987,
  "count": 4,
  "profile": "ARMED",
  "epoch_utc": 1765914600
}
```

### Fält

- `pir_event_id` (int, required)
- `profile` (string, required)
- `src_mask` (int, recommended ny stil)
- `pir` (int, legacy)
- `count` (int, optional)
- tidsfält (recommended)

### Rekommendation

- Ny integration bör använda `src_mask`.
- Legacy-stöd för `pir` kan finnas kvar tills alla flöden är uppdaterade.

---

## 8) Event ACK – `van/ellie/cmd/ack`

**Status:** aktiv

### Syfte

HA eller Node-RED bekräftar att ett PIR-event tagits emot och hanterats.

### Rekommenderad payload

```json
{
  "type": "PIR_ACK",
  "pir_event_id": 987
}
```

### Tolerant fallback

Device kan även acceptera `event_id` under migration:

```json
{
  "type": "PIR_ACK",
  "event_id": 987
}
```

### Fält

- `type` (string, required): `PIR_ACK`
- `pir_event_id` (int, required i ny spec)
- `event_id` (int, legacy fallback)

---

## 9) Version – `van/ellie/tele/version`

**Status:** rekommenderad men valfri som egen topic

### Syfte

Publicera firmware-/buildinformation separat.

### Exempel

```json
{
  "device_id": "ellie",
  "type": "VERSION",
  "fw": "2.2.0-dev",
  "build_date": "2026-03-08",
  "board": "LilyGO T-SIM7080G-S3",
  "gnss": "SparkFun NEO-F10N",
  "epoch_utc": 1772988927
}
```

---

## 10) Intervall och beteende

Detta avsnitt är vägledande och ska hållas synkat med firmware-/pipeline-dokumentationen.

Exempel från tidigare krav/logik:

- **PARKED**: `ALIVE` periodiskt, samt `GPS single` i samband med alive eller enligt egen policy
- **TRAVEL**: tätare GPS-rapportering
- **ARMED**: låg effekt / sleep-orienterat beteende, men PIR ska kunna väcka/logga
- **ARMED_AWAKE**: temporärt vaket läge efter PIR
- **TRIGGERED**: mer aktiv publicering / övervakning när alarm verkligen löst ut

> Exakta intervall hör i första hand hemma i firmware/systemdokumentationen. Payload-specen bör främst beskriva format, topic-användning och semantik.

---

## 11) Kompatibilitet med Home Assistant och Node-RED

### Home Assistant

- `device_tracker` eller motsvarande bör läsa `van/ellie/tele/gps`.
- Om `fix_ok=false` och `lat/lon` saknas ska integrationen tåla detta utan att skapa felaktig position.
- Profilbyte bör publiceras till `van/ellie/state/desired_profile` med `retain=true`.

### Node-RED

- Bör behandla `van/ellie/state/desired_profile` som faktisk desired state.
- Bör inte rensa retained state på den topicen efter ACK om den används som state-representation.
- Legacy-flöden som fortfarande använder `cmd/downlink` bör märkas tydligt som övergångslösning.

---

## 12) Rekommenderad migreringsplan

### Steg 1

Stöd både:
- `profile_change_id` och `ack_msg_id`
- `state/desired_profile` och `cmd/downlink`
- `src_mask` och `pir`
- `pir_event_id` och `event_id` i ACK-path där det behövs

### Steg 2

Uppdatera Home Assistant och Node-RED så att de primärt använder:
- `van/ellie/state/desired_profile`
- `profile_change_id`
- `src_mask`
- `pir_event_id`

### Steg 3

Nedgradera legacy till dokumenterad fallback:
- `cmd/downlink`
- `ack_msg_id` som primärt begrepp
- `pir`
- `event_id`

### Steg 4

När allt är migrerat kan legacy tas bort i firmware och i dokumentationens huvudflöde.

---

## 13) Skillnader mot tidigare spec

Viktiga uppdateringar i denna version:

1. **Primär styrtopic ändrad**  
   Från `van/ellie/cmd/downlink` till `van/ellie/state/desired_profile`.

2. **State-tänk i stället för command-tänk**  
   Desired profile ska normalt ligga retained som faktisk önskad state.

3. **Ny primär identifierare för profiländringar**  
   `profile_change_id` är primär. `ack_msg_id` finns kvar för migration.

4. **Gemensamma tidsfält i telemetri**  
   `timestamp`, `epoch_utc`, `time_valid`, `time_source`, `date_local`, `time_local`.

5. **GPS single prioriteras**  
   `batch` är legacy/reserverat tills det verkligen behövs.

6. **PIR-format moderniserat**  
   `src_mask` rekommenderas framför `pir: 1|2`.

---

## 14) Snabb sammanfattning

### Använd detta nu

- Profilstyrning: `van/ellie/state/desired_profile`
- Profil-ID: `profile_change_id`
- ACK från device: `van/ellie/ack`
- Telemetri: `tele/alive`, `tele/gps`, `tele/pir`
- Eventkvittens tillbaka: `van/ellie/cmd/ack`

### Behåll tillfälligt för kompatibilitet

- `van/ellie/cmd/downlink`
- `ack_msg_id`
- `pir`
- `event_id` i vissa fallback-flöden

---

## 15) Exempel – komplett normal profiländring

### 1. HA sätter desired profile

Topic: `van/ellie/state/desired_profile`

```json
{
  "profile_change_id": 201,
  "desired_profile": "ARMED",
  "source": "home_assistant"
}
```

### 2. Device kvitterar

Topic: `van/ellie/ack`

```json
{
  "device_id": "ellie",
  "type": "ACK",
  "profile_change_id": 201,
  "ack_msg_id": 201,
  "status": "OK",
  "detail": "profile_set",
  "profile": "ARMED",
  "epoch_utc": 1772989300
}
```

### 3. Device skickar alive i nya profilen

Topic: `van/ellie/tele/alive`

```json
{
  "device_id": "ellie",
  "msg_id": "130",
  "type": "ALIVE",
  "timestamp": "2026-03-08T17:01:40Z",
  "epoch_utc": 1772989300,
  "time_valid": true,
  "time_source": "NTP",
  "date_local": "2026-03-08",
  "time_local": "18:01:40",
  "profile": "ARMED",
  "uptime_s": 3720,
  "mqtt_ok": true,
  "fix_ok": true,
  "battery_v": 12.7,
  "moving": false
}
```

---

## 16) Ändringslogg

### v2.0 draft

- Primär profilstyrning flyttad till `state/desired_profile`
- `cmd/downlink` markerad som legacy/migration
- `profile_change_id` definierad som primär identifierare
- `ack_msg_id` kvar för bakåtkompatibilitet
- Gemensamma tidsfält lagda till i telemetri
- `GPS single` prioriterad framför batch
- PIR moderniserad med rekommenderad `src_mask`
- Fler konkreta payloadexempel tillagda

