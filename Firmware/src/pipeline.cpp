#include "pipeline.h"

#include "config.h"
#include "gps.h"
#include "logging.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "time_manager.h"

// ==============================
// PIR Outbox (server-ack driven)
// ==============================
struct PirOutbox
{
    bool pending = false;
    bool acked = false;
    uint32_t event_id = 0;
    uint16_t count = 0;
    uint32_t first_ms = 0;
    uint32_t last_ms = 0;
    uint8_t src_mask = 0; // bit0=front, bit1=back
};

// ---- Lockout: max 1 MQTT/min per PIR ----
static uint32_t g_pirIgnoreFrontUntilMs = 0;
static uint32_t g_pirIgnoreBackUntilMs = 0;
static const uint32_t PIR_LOCKOUT_MS = 60UL * 1000UL;

// ---- Filter: max 1 accepted trigger/s per PIR (för logg + vidare bearbetning) ----
static uint32_t g_lastPirAcceptedFrontMs = 0;
static uint32_t g_lastPirAcceptedBackMs = 0;
static const uint32_t PIR_ACCEPT_MIN_GAP_MS = 1000UL; // 1 Hz

static PirOutbox g_pir;

static volatile uint16_t g_pirIsrCount = 0;
static volatile uint8_t g_pirIsrMask = 0; // bit0=front, bit1=back

static uint32_t g_nextEventId = 1;      // TODO: persist i NVS senare
static bool g_alarmGpsSkipUsed = false; // skip GPS EN gång per ARMED-episod

// ---- ARMED_AWAKE window (30 min sliding, max 2 h) ----
static uint32_t g_armedAwakeStartMs = 0;
static uint32_t g_armedAwakeUntilMs = 0; // senaste "vaken till"
static bool g_armedAwakeActive = false;

static const uint32_t ARMED_AWAKE_WINDOW_MS = 30UL * 60UL * 1000UL;    // 30 min
static const uint32_t ARMED_AWAKE_MAX_MS = 2UL * 60UL * 60UL * 1000UL; // 2 h
static const uint32_t ARMED_AWAKE_COMM_MS = 2UL * 60UL * 1000UL;       // alive var 2 min
static uint32_t g_nextAwakeAliveAtMs = 0;

// ---- Publish-rate limiter: max 1/min per PIR ----
static uint32_t g_lastPirPublishFrontMs = 0;
static uint32_t g_lastPirPublishBackMs = 0;
static const uint32_t PIR_THROTTLE_MS = 60UL * 1000UL;

static void IRAM_ATTR isrPirFront()
{
    g_pirIsrCount++;
    g_pirIsrMask |= 0x01;
}

static void IRAM_ATTR isrPirBack()
{
    g_pirIsrCount++;
    g_pirIsrMask |= 0x02;
}

// ---------- helpers ----------
static inline bool isBefore(uint32_t nowMs, uint32_t untilMs)
{
    return (int32_t)(nowMs - untilMs) < 0;
}

static void pirStartOrExtendAwake(uint32_t nowMs)
{
    if (!g_armedAwakeActive)
    {
        g_armedAwakeActive = true;
        g_armedAwakeStartMs = nowMs;
        g_armedAwakeUntilMs = nowMs + ARMED_AWAKE_WINDOW_MS;
        g_nextAwakeAliveAtMs = nowMs;
    }
    else
    {
        uint32_t proposed = nowMs + ARMED_AWAKE_WINDOW_MS;
        uint32_t cap = g_armedAwakeStartMs + ARMED_AWAKE_MAX_MS;
        g_armedAwakeUntilMs = (proposed < cap) ? proposed : cap;
    }
}

// ==============================
// PIR ingest (ISR -> filtered -> lockout -> outbox)
// ==============================
static void pirIngestIsr(uint32_t nowMs)
{
    uint16_t n;
    uint8_t mask;

    noInterrupts();
    n = g_pirIsrCount;
    mask = g_pirIsrMask;
    g_pirIsrCount = 0;
    g_pirIsrMask = 0;
    interrupts();

    const auto &p = currentProfile();
    if (p.id != ProfileId::ARMED)
        return;

    if (n == 0 || mask == 0)
        return;

    // ---- Step 1: Lockout filter (ignorera helt under lockout) ----
    uint8_t lockoutFiltered = mask;

    if ((lockoutFiltered & 0x01) && isBefore(nowMs, g_pirIgnoreFrontUntilMs))
        lockoutFiltered &= ~0x01;

    if ((lockoutFiltered & 0x02) && isBefore(nowMs, g_pirIgnoreBackUntilMs))
        lockoutFiltered &= ~0x02;

    if (lockoutFiltered == 0)
    {
        // Allt låg i lockout -> ignorera tyst (eller logga vid behov)
        // logSystemf("PIR: LOCKOUT (ignored) mask=0x%02X", (unsigned)mask);
        return;
    }

    // ---- Step 2: 1 Hz accept per PIR (för att inte spamma logg & outbox) ----
    uint8_t acceptedMask = 0;

    // FRONT
    if (lockoutFiltered & 0x01)
    {
        if (g_lastPirAcceptedFrontMs == 0 ||
            (int32_t)(nowMs - g_lastPirAcceptedFrontMs) >= (int32_t)PIR_ACCEPT_MIN_GAP_MS)
        {
            acceptedMask |= 0x01;
            g_lastPirAcceptedFrontMs = nowMs;
        }
    }

    // BACK
    if (lockoutFiltered & 0x02)
    {
        if (g_lastPirAcceptedBackMs == 0 ||
            (int32_t)(nowMs - g_lastPirAcceptedBackMs) >= (int32_t)PIR_ACCEPT_MIN_GAP_MS)
        {
            acceptedMask |= 0x02;
            g_lastPirAcceptedBackMs = nowMs;
        }
    }

    if (acceptedMask == 0)
    {
        // Triggers kom, men vi tar max 1/s -> ignoreras
        // Logga sparsamt: här loggar vi varje gång vi *har ISR-data* men inget accepteras
        logSystemf("PIR: FILTERED (rate-limit 1Hz) raw_n=%u raw_mask=0x%02X", (unsigned)n, (unsigned)mask);
        return;
    }

    const char *which = (acceptedMask == 0x01)   ? "FRONT"
                        : (acceptedMask == 0x02) ? "BACK"
                        : (acceptedMask == 0x03) ? "FRONT+BACK"
                                                 : "UNKNOWN";

    // Detta är de triggers som “kommer vidare” (max 1/s per PIR) och syns i loggen
    logSystemf("PIR: FILTERED (accepted) which=%s raw_n=%u raw_mask=0x%02X accepted_mask=0x%02X",
               which, (unsigned)n, (unsigned)mask, (unsigned)acceptedMask);

    // ---- Step 3: Lägg i outbox (men queue:a inte för senare throttle; vi använder lockout för det) ----
    if (!g_pir.pending)
    {
        g_pir.pending = true;
        g_pir.acked = false;
        g_pir.event_id = g_nextEventId++;
        g_pir.count = 0;
        g_pir.first_ms = nowMs;
        g_pir.last_ms = nowMs;
        g_pir.src_mask = 0;
    }

    // Vi räknar 1 per accepterad “sekund-tick” per sensor (inte raw_n)
    // så count blir “antal sekunder med rörelse” ungefär, inte brus.
    uint16_t add = 0;
    if (acceptedMask & 0x01)
        add++;
    if (acceptedMask & 0x02)
        add++;

    g_pir.count = (uint16_t)(g_pir.count + add);
    g_pir.last_ms = nowMs;
    g_pir.src_mask |= acceptedMask;

    // Starta / förläng ARMED_AWAKE
    pirStartOrExtendAwake(nowMs);
}

// ==============================
// Pipeline state machine
// ==============================
enum class Step
{
    STEP_DECIDE = 0,
    STEP_GPS_ON,
    STEP_GPS_WARMUP,
    STEP_GPS_COLLECT,
    STEP_GPS_OFF,
    STEP_RF_ON,
    STEP_NET_ATTACH,
    STEP_MQTT_CONNECT,
    STEP_PUBLISH,
    STEP_RX_DOWNLINK,
    STEP_MQTT_DISCONNECT,
    STEP_RF_OFF,
    STEP_ALARM_WAIT,
    STEP_PARKED_WAIT
};

static Step g_step = Step::STEP_DECIDE;
static uint32_t g_stepEnterMs = 0;
static uint32_t g_deadlineMs = 0;
static uint32_t g_nextCommAtMs = 0;
static bool g_needComm = false;

// GPS result for this cycle
static bool g_gpsHave = false;
static bool g_gpsFixOk = false;
static GpsFix g_gpsFix;

enum class GpsPlan
{
    NONE = 0,
    SINGLE
};
static GpsPlan g_gpsPlan = GpsPlan::NONE;
static uint32_t g_gpsCollectTimeoutMs = 0;

// non-blocking GPS poll control
static uint32_t g_gpsNextPollMs = 0;
static uint32_t g_gpsPollIntervalMs = 1000UL; // adaptiv: snabbare när vi har kandidat

static void stepEnter(Step s, uint32_t nowMs)
{
    g_step = s;
    g_stepEnterMs = nowMs;

    switch (s)
    {
    case Step::STEP_DECIDE:
        break;

    case Step::STEP_GPS_ON:
        modemRfOff(); // GNSS <-> LTE mux: RF OFF innan GPS
        gpsPowerOn();
        g_deadlineMs = nowMs + 2000UL;
        break;

    case Step::STEP_GPS_WARMUP:
        g_deadlineMs = nowMs + 1500UL;
        break;

    case Step::STEP_GPS_COLLECT:
        g_deadlineMs = nowMs + g_gpsCollectTimeoutMs;
        g_gpsNextPollMs = nowMs;
        g_gpsPollIntervalMs = 1000UL;
        break;

    case Step::STEP_GPS_OFF:
        gpsPowerOff();
        g_deadlineMs = nowMs + 200UL;
        break;

    case Step::STEP_RF_ON:
        gpsPowerOff();
        g_deadlineMs = nowMs + 200UL;
        break;

    case Step::STEP_NET_ATTACH:
        g_deadlineMs = nowMs + 60000UL;
        break;

    case Step::STEP_MQTT_CONNECT:
        g_deadlineMs = nowMs + 15000UL;
        break;

    case Step::STEP_PUBLISH:
        g_deadlineMs = nowMs + 8000UL;
        break;

    case Step::STEP_RX_DOWNLINK:
        if (currentProfile().id == ProfileId::ARMED && g_armedAwakeActive)
        {
            g_deadlineMs = g_armedAwakeUntilMs;
        }
        else
        {
            g_deadlineMs = nowMs + (g_pir.pending ? 30000UL : 5000UL);
        }
        break;

    case Step::STEP_MQTT_DISCONNECT:
        mqttDisconnect();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_RF_OFF:
        modemRfOff();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_ALARM_WAIT:
    case Step::STEP_PARKED_WAIT:
        g_deadlineMs = g_nextCommAtMs;
        break;
    }
}

static bool stepTimedOut(uint32_t nowMs)
{
    return (int32_t)(nowMs - g_deadlineMs) >= 0;
}

// ------------------------------
// Helpers: PIR publish eligibility (per PIR, 1/min)
// ------------------------------
static bool pirCanPublishNow(uint32_t nowMs)
{
    if (!g_pir.pending)
        return false;

    bool allow = false;

    if (g_pir.src_mask & 0x01)
    {
        if (g_lastPirPublishFrontMs == 0 ||
            (int32_t)(nowMs - g_lastPirPublishFrontMs) >= (int32_t)PIR_THROTTLE_MS)
            allow = true;
    }

    if (g_pir.src_mask & 0x02)
    {
        if (g_lastPirPublishBackMs == 0 ||
            (int32_t)(nowMs - g_lastPirPublishBackMs) >= (int32_t)PIR_THROTTLE_MS)
            allow = true;
    }

    return allow;
}

static void pirMarkPublishedAndLockout(uint32_t nowMs)
{
    if (!g_pir.pending)
        return;

    // FRONT
    if (g_pir.src_mask & 0x01)
    {
        g_lastPirPublishFrontMs = nowMs;
        g_pirIgnoreFrontUntilMs = nowMs + PIR_LOCKOUT_MS;
    }

    // BACK
    if (g_pir.src_mask & 0x02)
    {
        g_lastPirPublishBackMs = nowMs;
        g_pirIgnoreBackUntilMs = nowMs + PIR_LOCKOUT_MS;
    }
}

// ==============================
// Hooks från andra moduler
// ==============================
void pipelineOnPirAck(uint32_t eventId)
{
    if (g_pir.pending && g_pir.event_id == eventId)
    {
        logSystemf("PIR: ACK -> clear outbox event_id=%u", (unsigned)eventId);

        // Clear direkt (även i ARMED_AWAKE där vi inte går via RF_OFF)
        g_pir.pending = false;
        g_pir.acked = false;
        g_pir.count = 0;
        g_pir.first_ms = 0;
        g_pir.last_ms = 0;
        g_pir.src_mask = 0;
    }
}

void pipelineOnProfileChanged(ProfileId newProfile)
{
    g_alarmGpsSkipUsed = false;
    (void)newProfile;

    if (newProfile != ProfileId::ARMED)
    {
        g_armedAwakeActive = false;
        g_armedAwakeStartMs = 0;
        g_armedAwakeUntilMs = 0;

        g_pir.pending = false;
        g_pir.acked = false;
        g_pir.count = 0;
        g_pir.src_mask = 0;

        // nollställ accept-filter också
        g_lastPirAcceptedFrontMs = 0;
        g_lastPirAcceptedBackMs = 0;
    }
}

// ==============================
// Init + tick
// ==============================
void pipelineInit()
{
    modemRfOff();
    gpsPowerOff();

    g_nextCommAtMs = millis() + 2000UL;

    pinMode(PIN_PIR_FRONT, INPUT_PULLDOWN);
    pinMode(PIN_PIR_BACK, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_FRONT), isrPirFront, RISING);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_BACK), isrPirBack, RISING);

    stepEnter(Step::STEP_DECIDE, millis());
}

void pipelineTick(uint32_t nowMs)
{
    // Ingest PIR varje tick (oavsett step)
    pirIngestIsr(nowMs);

    switch (g_step)
    {
    case Step::STEP_DECIDE:
    {
        const auto &p = currentProfile();

        if (g_armedAwakeActive && (int32_t)(nowMs - g_armedAwakeUntilMs) >= 0)
            g_armedAwakeActive = false;

        bool commDue = ((int32_t)(nowMs - g_nextCommAtMs) >= 0);
        g_needComm = g_pir.pending || commDue;

        // Reset per cycle gps result
        g_gpsHave = false;
        g_gpsFixOk = false;
        g_gpsFix = GpsFix{};

        // GPS-plan
        g_gpsPlan = GpsPlan::NONE;
        g_gpsCollectTimeoutMs = 0;

        if (g_needComm)
        {
            if (p.id == ProfileId::ARMED)
            {
                if (g_pir.pending && !g_alarmGpsSkipUsed)
                {
                    g_gpsPlan = GpsPlan::NONE;
                    g_alarmGpsSkipUsed = true;
                }
                else
                {
                    g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                    if (p.gpsFixWaitMs > 0)
                        g_gpsCollectTimeoutMs = p.gpsFixWaitMs;
                }
            }
            else
            {
                g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                if (p.gpsFixWaitMs > 0)
                    g_gpsCollectTimeoutMs = p.gpsFixWaitMs;
            }
        }

        if (!g_needComm)
        {
            if (p.id == ProfileId::ARMED)
                stepEnter(Step::STEP_ALARM_WAIT, nowMs);
            else
                stepEnter(Step::STEP_PARKED_WAIT, nowMs);
            break;
        }

        if (g_gpsPlan != GpsPlan::NONE)
            stepEnter(Step::STEP_GPS_ON, nowMs);
        else
            stepEnter(Step::STEP_RF_ON, nowMs);

        break;
    }

    // ---------------- GPS ----------------
    case Step::STEP_GPS_ON:
        stepEnter(Step::STEP_GPS_WARMUP, nowMs);
        break;

    case Step::STEP_GPS_WARMUP:
        if (stepTimedOut(nowMs))
            stepEnter(Step::STEP_GPS_COLLECT, nowMs);
        break;

    case Step::STEP_GPS_COLLECT:
    {
        if ((int32_t)(nowMs - g_gpsNextPollMs) >= 0)
        {
            g_gpsNextPollMs = nowMs + g_gpsPollIntervalMs;

            GpsFix fx;
            bool ok = gpsPollOnce(fx);
            if (ok)
            {
                if (fx.candidate && !fx.valid)
                    g_gpsPollIntervalMs = 500UL;

                if (fx.valid)
                {
                    g_gpsFix = fx;
                    g_gpsFixOk = true;
                    g_gpsHave = true;
                    stepEnter(Step::STEP_GPS_OFF, nowMs);
                    break;
                }
            }
        }

        if (stepTimedOut(nowMs))
        {
            g_gpsHave = false;
            g_gpsFixOk = false;
            logSystem("GPS: timeout (no valid fix)");
            stepEnter(Step::STEP_GPS_OFF, nowMs);
        }
        break;
    }

    case Step::STEP_GPS_OFF:
        stepEnter(Step::STEP_RF_ON, nowMs);
        break;

    // ---------------- RF + NET + MQTT ----------------
    case Step::STEP_RF_ON:
        stepEnter(Step::STEP_NET_ATTACH, nowMs);
        break;

    case Step::STEP_NET_ATTACH:
    {
        NetResult net;
        bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);
        if (ok)
        {
            timeSyncFromModem();
            timeSyncFromNtp(8000);
            stepEnter(Step::STEP_MQTT_CONNECT, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            const auto &p = currentProfile();
            g_nextCommAtMs = nowMs + p.commIntervalMs;
            stepEnter(Step::STEP_RF_OFF, nowMs);
        }
        break;
    }

    case Step::STEP_MQTT_CONNECT:
        if (mqttConnect())
        {
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }
        if (stepTimedOut(nowMs))
        {
            const auto &p = currentProfile();
            g_nextCommAtMs = nowMs + p.commIntervalMs;
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
        }
        break;

    case Step::STEP_PUBLISH:
    {
        if (g_gpsHave)
            mqttPublishGpsSingle(g_gpsFix, g_gpsFixOk);

        // PIR: publish om tillåtet just nu, annars DROPPAR vi (lockout-mode, ingen kö)
        if (g_pir.pending)
        {
            if (pirCanPublishNow(nowMs))
            {
                pirMarkPublishedAndLockout(nowMs);
                mqttPublishPirEvent(g_pir.event_id, g_pir.count, g_pir.first_ms, g_pir.last_ms, g_pir.src_mask);
            }
            else
            {
                // Viktigt: INGEN “queue till nästa minut”
                logSystem("PIR: blocked by 1/min rule -> drop (lockout mode, no queue)");
                g_pir.pending = false;
                g_pir.acked = false;
                g_pir.count = 0;
                g_pir.first_ms = 0;
                g_pir.last_ms = 0;
                g_pir.src_mask = 0;
            }
        }

        mqttPublishAlive();

        stepEnter(Step::STEP_RX_DOWNLINK, nowMs);
        break;
    }

    case Step::STEP_RX_DOWNLINK:
    {
        mqttLoop();

        if (currentProfile().id == ProfileId::ARMED && g_armedAwakeActive)
        {
            // Ny PIR pending under RX: om 1/min-regeln tillåter, publicera direkt.
            // Om inte tillåter => droppa direkt så den inte “skjuts upp”.
            if (g_pir.pending)
            {
                if (pirCanPublishNow(nowMs))
                {
                    stepEnter(Step::STEP_PUBLISH, nowMs);
                    break;
                }
                else
                {
                    logSystem("PIR: pending during RX but blocked by 1/min -> drop");
                    g_pir.pending = false;
                    g_pir.acked = false;
                    g_pir.count = 0;
                    g_pir.first_ms = 0;
                    g_pir.last_ms = 0;
                    g_pir.src_mask = 0;
                }
            }

            // Periodisk alive (2 min)
            if ((int32_t)(nowMs - g_nextAwakeAliveAtMs) >= 0)
            {
                mqttPublishAlive();
                g_nextAwakeAliveAtMs = nowMs + ARMED_AWAKE_COMM_MS;
            }

            if ((int32_t)(nowMs - g_armedAwakeUntilMs) >= 0)
            {
                g_armedAwakeActive = false;
                stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            }
            break;
        }

        if (stepTimedOut(nowMs))
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);

        break;
    }

    case Step::STEP_MQTT_DISCONNECT:
        stepEnter(Step::STEP_RF_OFF, nowMs);
        break;

    case Step::STEP_RF_OFF:
    {
        const auto &p = currentProfile();
        uint32_t effectiveCommMs = p.commIntervalMs;
        if (p.id == ProfileId::ARMED && g_armedAwakeActive)
            effectiveCommMs = ARMED_AWAKE_COMM_MS;

        g_nextCommAtMs = nowMs + effectiveCommMs;

        if (p.id == ProfileId::ARMED)
            stepEnter(Step::STEP_ALARM_WAIT, nowMs);
        else
            stepEnter(Step::STEP_PARKED_WAIT, nowMs);
        break;
    }

    case Step::STEP_ALARM_WAIT:
    {
        if (g_pir.pending)
        {
            g_nextCommAtMs = nowMs;
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }
        if ((int32_t)(nowMs - g_nextCommAtMs) >= 0)
            stepEnter(Step::STEP_DECIDE, nowMs);
        break;
    }

    case Step::STEP_PARKED_WAIT:
    {
        if ((int32_t)(nowMs - g_nextCommAtMs) >= 0)
            stepEnter(Step::STEP_DECIDE, nowMs);
        break;
    }

    default:
        stepEnter(Step::STEP_DECIDE, nowMs);
        break;
    }
}