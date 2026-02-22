# Campervanlarm – Uppföljning (kravstatus, test, resultat)
Utgår från `docs/01_krav.md` (v5). Här följer vi upp vad som är implementerat, vad som återstår och vad som är verifierat med test.

## Statuskoder
- **EJ** = Ej påbörjat
- **PÅG** = Pågående
- **IMP** = Implementerat (kod finns)
- **VER** = Verifierat (testat och bevisat)

## Definitioner (kort)
- **Implementation**: fil(er)/modul/commit/branch där kravet är implementerat.
- **Test**: hur kravet testats (bänk/van/långtest) + länk/logg.
- **Resultat**: datum + kort utfall + ev avvikelse.

---

## Kravspårning

### 1) Mode-styrning och synk mot HA
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-001 | Stöd UC-01..UC-04 | EJ |  |  |  |  |
| KR-002 | Lägesbyten via MQTT (HA master) | EJ |  |  |  |  |
| KR-003 | Mode-sync vid uppstart (≤60s) | EJ |  |  |  |  |
| KR-004 | HA mode retained | EJ |  |  |  |  |
| KR-005 | Check mode vid alive/position-cykel | EJ |  |  |  |  |
| KR-006 | Mode-ack tillbaka till HA | EJ |  |  |  |  |

### 2) PIR och larm-event
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-010 | PIR aktiv endast i ARMED | EJ |  |  |  |  |
| KR-011 | PIR -> MQTT event (≤5s efter MQTT connect) | EJ |  |  |  |  |
| KR-012 | PIR loggas till Serial i dev | EJ |  |  |  |  |
| KR-013 | PIR i ARMED -> ARMED_AWAKE (MQTT uppe ≥30 min) | EJ |  |  |  |  |
| KR-014 | ARMED_AWAKE alive var 2 min | EJ |  |  |  |  |
| KR-015 | Sliding window på ARMED_AWAKE | EJ |  |  |  |  |
| KR-016 | Max-tak ARMED_AWAKE 2 h | EJ |  |  |  |  |
| KR-017 | Throttle PIR: max 1/min per PIR | EJ |  |  |  |  |

### 3) GNSS – extern position
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-020 | Läs GNSS + fix_ok | EJ |  |  |  |  |
| KR-021 | UC-01: single vid varje alive (även utan fix) | EJ |  |  |  |  |
| KR-022 | UC-02: batch var 5 min, 30 pkt à 10 s | EJ |  |  |  |  |
| KR-023 | UC-03: single var 15 s | EJ |  |  |  |  |
| KR-024 | Fungerar utan GNSS-fix | EJ |  |  |  |  |
| KR-025 | UC-01: GNSS alltid aktiv | EJ |  |  |  |  |
| KR-026 | UC-01: single utan fix (val A) | EJ |  |  |  |  |

### 4) Kommunikation – MQTT
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-030 | Alive-intervall per läge (v5) | EJ |  |  |  |  |
| KR-031 | UC-03: MQTT konstant uppe (reconnect-loop) | EJ |  |  |  |  |
| KR-032 | UC-01: reconnect “normalt” vid tapp | EJ |  |  |  |  |
| KR-033 | MQTT kommandon (mode-set etc) | EJ |  |  |  |  |
| KR-034 | Payload versionshantering | EJ |  |  |  |  |

### 5) Energi / sleep / wake
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-040 | Deep sleep minst i UC-04 | EJ |  |  |  |  |
| KR-041 | Timer wake | EJ |  |  |  |  |
| KR-042 | PIR wake i UC-04 | EJ |  |  |  |  |
| KR-043 | Energi-mål per UC (senare) | EJ |  |  |  |  |

### 6) Loggning och felsökning
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KR-050 | Viktiga händelser till Serial (dev) | EJ |  |  |  |  |
| KR-051 | Loggformat konsekvent | EJ |  |  |  |  |

### 7) Icke-funktionella krav
| ID | Krav (kort) | Status | Implementation (fil/commit) | Test | Resultat | Notering |
|---|---|---|---|---|---|---|
| KNF-001 | Stabil drift 72 h | EJ |  |  |  |  |
| KNF-002 | Återhämtning efter 30 min nätbortfall | EJ |  |  |  |  |
| KNF-003 | Inga hemligheter i repo | EJ |  |  |  |  |

---

## Testlogg (kort)
| Datum | Test | Miljö | Utfall | Logg/Ref |
|---|---|---|---|---|
|  |  |  |  |  |

---

## Kända problem / risker
| ID | Risk/Problem | Påverkan | Status | Åtgärd |
|---|---|---|---|---|
| R-001 |  |  | Öppen |  |
| R-002 |  |  | Öppen |  |

---

## Nästa steg (max 5)
1. Importera/synka firmware-koden till `firmware/` och bygga baseline.
2. Implementera mode-sync (retained downlink + ack) och verifiera med HA `van.yaml`.
3. Implementera extern GNSS single (UC-01/UC-03) och verifiera HA tracker + fix-sensorer.
4. Implementera batch i RAM-ringbuffer (UC-02) och verifiera Node-RED/HA tolkar korrekt.
5. Implementera ARMED_AWAKE (30 min, sliding window, max 2h, throttle 1/min per PIR) och verifiera end-to-end.

