# campervanlarm

Larm för fordon med hjälp av LilyGO T-SIM7080G-S3 (SIM7080G LTE-M/NB-IoT) kombinerat med extern GNSS (GPS/GNSS-modul).
Projektet innehåller firmware, Home Assistant-konfiguration och Node-RED-flöden samt dokumentation.

## Funktioner (översikt)
- Profiler: PARKED / TRAVEL / ALARM
- PIR-sensorer (intrång)
- Extern GNSS: position (single + batch)
- MQTT-integration mot Home Assistant / Node-RED
- Batterimätning och periodiskt alive/status
- SD-loggning (valfritt)
- Deep sleep / wake via PIR eller timer (beroende på profil)

## Repo-struktur
- `firmware/` – PlatformIO-projekt för T-SIM7080G-S3
- `home-assistant/` – HA packages/dashboards/blueprints
- `node-red/` – Node-RED flows + funktioner
- `docs/` – krav, uppföljning, implementation + payloadspec

## Kom igång (firmware)
### Krav
- VS Code + PlatformIO
- USB-drivrutiner för din board (om behövs)

### 1) Klona repot
```bash
git clone https://github.com/csspel/campervanlarm.git
cd campervanlarm
