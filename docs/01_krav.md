# Campervanlarm – Kravspecifikation (v5)
**Scope:** Fordonslarm baserat på T-SIM7080G-S3 (ESP32-S3 + SIM7080G) med **extern GNSS**.  
**Styrning:** Home Assistant (HA) är “master” och styr läge via MQTT.

---

## 1. Målbild
Systemet ska:
1) rapportera status (alive) och batteri,
2) rapportera position (single/batch/continuous beroende på läge),
3) i *armed*-läge kunna trigga larm utifrån PIR,
4) synka läge mot HA vid uppstart och vid kommunikationstillfällen,
5) fungera stabilt och förutsägbart över tid.

---

## 2. Definitioner
- **Läge (UC):** Aktivt beteende som styr hur systemet arbetar.
- **UC-01 PARKED (disarmed):** Alive var 5 min + single GPS vid varje alive. GNSS hålls aktiv.
- **UC-02 TRAVEL (disarmed):** Batch var 5 min (30 punkter à 10 s) + alive var 5 min.
- **UC-03 TRIGGERED:** Sätts av HA när larm utlöst. Single GPS var 15 s, MQTT hålls uppe.
- **UC-04 ARMED:** Sensorer aktiva. PIR kan trigga larm-event. Normalt långsleep, alive var 30 min.

- **ARMED_AWAKE window:** 30 min “vaken och lyssnar” efter PIR i UC-04. Alive var 2 min.  
  Sliding window förlängs av nya PIR-triggers, max totalt 2 timmar.

- **Alive:** Statusmeddelande (läge, batteri, fixstatus, mm).
- **Single GPS:** Ett positionsmeddelande.
- **Batch GPS:** Ett paket med flera positioner.
- **Continuous GPS:** Återkommande single GPS med fast intervall tills HA byter läge.

---

## 3. Antaganden och avgränsningar
- LTE-M / NB-IoT via SIM7080G.
- MQTT används mot HA/Node-RED.
- Extern GNSS ansluts till ESP32-S3 (UART/I2C).
- Ingen siren i v1 (kan läggas till senare).
- Inga automatiska lägesbyten i v1 (HA styr allt).
- Ingen SD-loggning (SD har strulat tidigare).

---

## 4. Use cases / Lägen
### UC-01: PARKED (disarmed)
- Alive var 5 min.
- Vid varje alive skickas single GPS.
- GNSS hålls aktiv kontinuerligt (ingen duty cycle).
- MQTT hålls uppe “normalt” och reconnectar vid tapp (inte krav på konstant uppkoppling).

**Single GPS utan fix (val A):** Om fix saknas skickas single GPS ändå med `fix_ok=false` och utan lat/lon (eller med null/utelämnat enligt payloadspec), så att HA ser att enheten försöker.

### UC-02: TRAVEL (disarmed)
- Batch skickas var 5 min med 30 punkter à 10 s.
- Alive skickas samtidigt var 5 min.
- Single GPS behövs inte i UC-02.
- Batch-data lagras i RAM ringbuffer tills den är sänd (och försvinner vid reboot).

### UC-03: TRIGGERED
- Sätts av HA via MQTT när larm triggas.
- Systemet skickar single GPS var 15 s.
- Alive skickas samtidigt som varje single GPS.
- Systemet ska försöka hålla MQTT-uppkopplingen konstant (reconnect-loop vid tapp).

### UC-04: ARMED
- Primärt “vinterläge”: alive var 30 min och i övrigt deep sleep.
- Väckning via timer och via PIR (GPIO wake).
- PIR är aktivt endast i UC-04.
- Vid PIR triggar: systemet går in i ARMED_AWAKE window i 30 min och håller MQTT uppkopplad.

### ARMED_AWAKE window (del av UC-04)
- Varaktighet: 30 min från senaste PIR (sliding window).
- Max total vaken-tid: 2 timmar (tak).
- Alive-intervall: 2 min.
- PIR-event throttlas: max 1 event per minut **per PIR** till MQTT (men kan fortfarande serial-loggas för utveckling).

---

## 5. Krav (med acceptanskriterier)
Prioritet: **Must / Should / Could**

### 5.1 Mode-styrning och synk mot HA
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-001 | Systemet ska stödja UC-01..UC-04 | Must | Läge rapporteras i Alive |
| KR-002 | Alla lägesbyten ska ske via MQTT-kommando från HA | Must | Läge ändras endast av MQTT-kommando |
| KR-003 | Vid uppstart ska systemet synka läge mot HA | Must | Inom 60 s efter boot ska läge vara synkat |
| KR-004 | Mode i HA ska publiceras som retained | Must | Enheten får korrekt läge direkt efter MQTT connect |
| KR-005 | Systemet ska kontrollera om nytt läge finns i samband med Alive/position-publicering | Must | Vid varje publish-cykel görs check på mode-topic |
| KR-006 | Systemet ska kunna bekräfta (ack) mottaget läge | Should | HA kan se att enheten bytt läge |

### 5.2 PIR och larm-event
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-010 | PIR ska endast vara aktiv i UC-04 ARMED | Must | I andra UC ignoreras PIR |
| KR-011 | PIR-trigg i UC-04 ska publicera larm-event till HA | Must | Event publiceras inom 5 s efter MQTT connect |
| KR-012 | PIR-trigg ska loggas till Serial i utvecklingsfas | Must | Loggrad med tid + sensor-id |
| KR-013 | Vid PIR i UC-04 ska systemet hålla MQTT uppkopplad i minst 30 min (ARMED_AWAKE) | Must | Under 30 min ska kommandon kunna tas emot |
| KR-014 | Under ARMED_AWAKE ska alive skickas var 2 min | Must | Intervall 2 min ± tolerans |
| KR-015 | ARMED_AWAKE ska förlängas vid ny PIR (sliding window) | Must | Timern flyttas fram vid ny PIR |
| KR-016 | ARMED_AWAKE ska ha max-tak 2 timmar | Must | Vakenperiod kan inte överstiga 2 timmar totalt |
| KR-017 | PIR-event ska throttlas till max 1/min per PIR | Must | MQTT får inte fler än 1 event/min per PIR |

### 5.3 GNSS – extern position
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-020 | Systemet ska kunna läsa GNSS och avgöra fix ok | Must | Fixstatus i Alive och/eller GPS payload |
| KR-021 | UC-01 ska skicka single GPS vid varje alive | Must | Single publiceras var 5 min (även utan fix) |
| KR-022 | UC-02 ska skicka batch var 5 min med 30 punkter à 10 s | Must | Batch innehåller tid + lat/lon per punkt (om fix finns) |
| KR-023 | UC-03 ska skicka single GPS var 15 s | Must | 15 s ± tolerans |
| KR-024 | Systemet ska fungera utan GNSS-fix | Must | Alive + event fungerar även med fix_ok=false |
| KR-025 | I UC-01 ska GNSS vara kontinuerligt aktiv | Must | GNSS stängs inte av mellan alive |
| KR-026 | UC-01 single GPS utan fix ska skickas (val A) | Must | fix_ok=false och lat/lon utelämnade/null |

### 5.4 Kommunikation – MQTT
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-030 | Alive ska skickas med definierat intervall per läge | Must | UC-01: 5 min, UC-02: 5 min, UC-03: 15 s, UC-04: 30 min, ARMED_AWAKE: 2 min |
| KR-031 | UC-03 ska försöka hålla MQTT uppkopplad konstant | Must | Vid tapp: reconnect-loop och fortsatt sändning när uppe |
| KR-032 | Systemet ska reconnecta vid tapp i UC-01 “normalt” | Must | Återanslutning sker utan manuell reset i normalfall |
| KR-033 | Systemet ska kunna ta emot kommandon via MQTT | Must | Mode-set ska fungera i alla UC där MQTT är aktivt |
| KR-034 | Payload-format ska vara versionshanterat | Should | Innehåller version/msg_id |

### 5.5 Energi / sleep / wake
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-040 | Deep sleep ska stödjas minst i UC-04 | Must | Enheten sover mellan wake-cykler |
| KR-041 | Wake via timer ska fungera | Must | Timer wake -> alive/ev position |
| KR-042 | Wake via PIR (GPIO) ska fungera i UC-04 | Must | PIR wake -> event + ARMED_AWAKE |
| KR-043 | Energi-mål ska definieras per UC (senare) | Should | Dokumenterade mål + verifieringsplan |

### 5.6 Loggning och felsökning
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KR-050 | Viktiga händelser ska loggas till Serial i utvecklingsfas | Must | boot, mode-sync, mode-change, PIR, MQTT connect, GNSS fix |
| KR-051 | Loggformat ska vara konsekvent | Should | timestamp + modul + nivå |

### 5.7 Icke-funktionella krav
| ID | Krav | Prio | Acceptanskriterium |
|---|---|---|---|
| KNF-001 | Stabil drift över tid | Must | 72 h test utan låsning i normal användning |
| KNF-002 | Tålighet mot dålig täckning | Must | Återhämtar sig efter 30 min nätbortfall |
| KNF-003 | Inga hemligheter i publikt repo | Must | Secrets i ignorerade filer + example-mallar |

---

## 6. Teststrategi (översikt)
- Bänk: MQTT connect/reconnect, retained mode, GNSS, PIR.
- Van stilla: UC-01/UC-04 sleep/wake, alive, fixbeteende.
- Van resa: UC-02 batch och täckningsdrop.
- Larmtest: UC-04 PIR -> event -> HA sätter UC-03 -> 15 s GPS.

---

## 7. Öppna frågor (kvar att spika)
1) Exakt topic-struktur och payloadspec låses i `docs/mqtt_payload_spec.md`.
2) Ska batchpunkter innehålla speed/course/alt eller bara lat/lon/tid?
3) Hur ska tidskällan prioriteras (GNSS-tid vs NTP via modem)?
