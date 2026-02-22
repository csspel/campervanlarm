#include "pipeline.h"
#include "config.h"
#include "logging.h"
#include "modem.h"
#include "mqtt.h"
#include "gps.h"
#include "time_manager.h"
#include "profiles.h"

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

static PirOutbox g_pir;
static volatile uint16_t g_pirIsrCount = 0;
static volatile uint8_t g_pirIsrMask = 0; // bit0=front, bit1=back
static uint32_t g_nextEventId = 1;        // TODO: persist i NVS/SD senare
static bool g_alarmGpsSkipUsed = false;   // skip GPS EN gång per ALARM-episod

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

    if (n == 0)
        return;

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

    g_pir.count = (uint16_t)(g_pir.count + n);
    g_pir.last_ms = nowMs;
    g_pir.src_mask |= mask;
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
        g_deadlineMs = nowMs + 1500UL; // 1.5 s settle time
        break;

    case Step::STEP_GPS_COLLECT:
        g_deadlineMs = nowMs + g_gpsCollectTimeoutMs;
        g_gpsNextPollMs = nowMs;      // poll direkt
        g_gpsPollIntervalMs = 1000UL; // börja lugnt
        break;

    case Step::STEP_GPS_OFF:
        gpsPowerOff();
        g_deadlineMs = nowMs + 200UL;
        break;

    case Step::STEP_RF_ON:
        gpsPowerOff(); // säkerställ GNSS OFF innan RF
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
        // längre rx-fönster om PIR väntar på ack
        g_deadlineMs = nowMs + (g_pir.pending ? 30000UL : 5000UL);
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
        g_deadlineMs = nowMs + 4UL * 60UL * 1000UL;
        break;

    case Step::STEP_PARKED_WAIT:
        g_deadlineMs = g_nextCommAtMs;
        break;
    }
}

static bool stepTimedOut(uint32_t nowMs)
{
    return (int32_t)(nowMs - g_deadlineMs) >= 0;
}

// ==============================
// Hooks från andra moduler
// ==============================
void pipelineOnPirAck(uint32_t eventId)
{
    if (g_pir.pending && g_pir.event_id == eventId)
    {
        g_pir.acked = true;
    }
}

void pipelineOnProfileChanged(ProfileId newProfile)
{
    // Nollställ “skip GPS en gång” vid profilbyte
    g_alarmGpsSkipUsed = false;
    (void)newProfile;
}

// ==============================
// Init + tick
// ==============================
void pipelineInit()
{
    // Starta med RF & GPS av
    modemRfOff();
    gpsPowerOff();

    g_nextCommAtMs = millis() + 2000UL;

    // PIR
    pinMode(PIN_PIR_FRONT, INPUT);
    pinMode(PIN_PIR_BACK, INPUT);
    int mode = PIR_RISING_EDGE ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_FRONT), isrPirFront, mode);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_BACK), isrPirBack, mode);

    stepEnter(Step::STEP_DECIDE, millis());
}

void pipelineTick(uint32_t nowMs)
{
    // Ingest PIR varje tick (oavsett step)
    pirIngestIsr(nowMs);

    switch (g_step)
    {
    // ---------------- DECIDE ----------------
    case Step::STEP_DECIDE:
    {
        const auto &p = currentProfile();
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
            if (p.id == ProfileId::ALARM)
            {
                // Skip GPS exakt en gång per ALARM-episod (när PIR pending första gången)
                if (g_pir.pending && !g_alarmGpsSkipUsed)
                {
                    g_gpsPlan = GpsPlan::NONE;
                    g_alarmGpsSkipUsed = true;
                }
                else
                {
                    g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                    if (p.gpsFixWaitMs > 0)
                    {
                        uint32_t waitMs = p.gpsFixWaitMs;
                        if (!gpsHasLastFix() && waitMs < 300000UL)
                            waitMs = 300000UL; // 5 min
                        g_gpsCollectTimeoutMs = waitMs;
                    }
                }
            }
            else
            {
                // PARKED/TRAVEL: single fix om fixWaitMs > 0
                g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                if (p.gpsFixWaitMs > 0)
                {
                    uint32_t waitMs = p.gpsFixWaitMs;
                    if (!gpsHasLastFix() && waitMs < 300000UL)
                        waitMs = 300000UL; // 5 min
                    g_gpsCollectTimeoutMs = waitMs;
                }
            }
        }

        if (!g_needComm)
        {
            // Vänta enligt profil
            if (p.id == ProfileId::ALARM)
                stepEnter(Step::STEP_ALARM_WAIT, nowMs);
            else
                stepEnter(Step::STEP_PARKED_WAIT, nowMs);
            break;
        }

        // Kör pipeline
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
        // Non-blocking: poll CGNSINF tills valid (stabilitet+quality gate) eller deadline.
        if ((int32_t)(nowMs - g_gpsNextPollMs) >= 0)
        {
            g_gpsNextPollMs = nowMs + g_gpsPollIntervalMs;

            GpsFix fx;
            bool ok = gpsPollOnce(fx);

            if (ok)
            {
                // Adaptiv poll: när vi ser kandidat, poll:a snabbare så vi snabbare får 2 stabila prover
                if (fx.candidate && !fx.valid)
                {
                    g_gpsPollIntervalMs = 500UL;
                }

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
            // timeout: fortsätt ändå utan GPS
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
            // sync time best effort
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
        // GPS (single)
        if (g_gpsHave)
        {
            mqttPublishGpsSingle(g_gpsFix, g_gpsFixOk);
        }

        // PIR event (outbox rensas INTE här)
        if (g_pir.pending)
        {
            mqttPublishPirEvent(g_pir.event_id, g_pir.count, g_pir.first_ms, g_pir.last_ms, g_pir.src_mask);
        }

        // Alive
        mqttPublishAlive();

        stepEnter(Step::STEP_RX_DOWNLINK, nowMs);
        break;

    case Step::STEP_RX_DOWNLINK:
        mqttLoop();
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
        }
        break;

    case Step::STEP_MQTT_DISCONNECT:
        stepEnter(Step::STEP_RF_OFF, nowMs);
        break;

    case Step::STEP_RF_OFF:
    {
        // Rensa PIR endast på server-ack
        if (g_pir.pending && g_pir.acked)
        {
            logSystem("PIR: event acked, clearing outbox");
            g_pir.pending = false;
            g_pir.acked = false;
            g_pir.count = 0;
        }
        else if (g_pir.pending)
        {
            logSystem("PIR: pending without ack (will retry later)");
        }

        // Schemalägg nästa comm enligt profil
        const auto &p = currentProfile();
        g_nextCommAtMs = nowMs + p.commIntervalMs;

        // Gå till WAIT enligt profil
        if (p.id == ProfileId::ALARM)
            stepEnter(Step::STEP_ALARM_WAIT, nowMs);
        else
            stepEnter(Step::STEP_PARKED_WAIT, nowMs);
        break;
    }

    // ---------------- WAIT ----------------
    case Step::STEP_ALARM_WAIT:
        if (g_pir.pending)
        {
            g_nextCommAtMs = nowMs; // force immediate comm
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
        }
        break;

    case Step::STEP_PARKED_WAIT:
        if ((int32_t)(nowMs - g_nextCommAtMs) >= 0)
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
        }
        break;
    }
}
