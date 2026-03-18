#include "pipeline.h"

#include "config.h"
#include "ext_gnss.h"
#include "logging.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "time_manager.h"

// ============================================================
// PIR outbox
// ------------------------------------------------------------
// Här samlar vi ihop PIR-triggers som ska publiceras via MQTT.
// pending  = det finns ett event som ännu inte är färdigbehandlat
// event_id = unikt id för aktuell PIR-händelse
// count    = antal accepterade PIR-triggers i samma event
// first_ms = första tidpunkt för eventet
// last_ms  = senaste tidpunkt för eventet
// src_mask = bitmask för källa:
//            bit0 = front
//            bit1 = back
// ============================================================
struct PirOutbox
{
    bool pending = false;
    uint32_t event_id = 0;
    uint16_t count = 0;
    uint32_t first_ms = 0;
    uint32_t last_ms = 0;
    uint8_t src_mask = 0; // bit0=front, bit1=back
};

static PirOutbox g_pir;

// ============================================================
// PIR filter / lockout
// ------------------------------------------------------------
// Vi har två nivåer:
//
// 1) ACCEPT_MIN_GAP:
//    Stoppar studs / alltför täta triggers från samma sensor.
//
// 2) THROTTLE + LOCKOUT efter lyckad publish:
//    Begränsar hur ofta PIR-event får skickas och ignorerar nya
//    triggers en stund efter lyckad publicering.
// ============================================================
static uint32_t g_lastPirAcceptedFrontMs = 0;
static uint32_t g_lastPirAcceptedBackMs = 0;
static const uint32_t PIR_ACCEPT_MIN_GAP_MS = 1000UL;

static uint32_t g_lastPirPublishFrontMs = 0;
static uint32_t g_lastPirPublishBackMs = 0;
static const uint32_t PIR_THROTTLE_MS = 60UL * 1000UL;

static uint32_t g_pirIgnoreFrontUntilMs = 0;
static uint32_t g_pirIgnoreBackUntilMs = 0;
static const uint32_t PIR_LOCKOUT_MS = 60UL * 1000UL;

// ============================================================
// PIR ISR-state
// ------------------------------------------------------------
// ISR:n gör minsta möjliga:
// - ökar räknare
// - sätter bit i mask
//
// Själva filtrering/publiceringslogik sker i vanlig pipeline-kod.
// ============================================================
static volatile uint16_t g_pirIsrCount = 0;
static volatile uint8_t g_pirIsrMask = 0; // bit0=front, bit1=back

// ============================================================
// Övrigt state
// ============================================================
static uint32_t g_nextEventId = 1;
static uint32_t g_nextCommAtMs = 0;

// När TRIGGERED ska återgå till ARMED.
static uint32_t g_triggeredUntilMs = 0;

// När profil just har ändrats och vi redan har MQTT uppe,
// ska vi hålla anslutningen uppe tills minst en publish-cykel
// hunnit gå med nya profilen.
static bool g_profileChangePublishPending = false;

// ------------------------------------------------------------
// Boot-profile-sync state
// ------------------------------------------------------------
// När vi precis kopplat upp MQTT efter boot/reconnect vill vi ge
// retained desired profile en chans att komma in innan vi kör första
// publish-cykeln och ev. kopplar ner igen.
//
// g_bootProfileSyncActive:
//   true under det korta sync-fönstret efter MQTT connect.
//
// Detta används också i pipelineOnProfileChanged() för att undvika
// att en profil som inte är keepConnected direkt orsakar disconnect
// innan första publish hunnit ske.
// ------------------------------------------------------------
static bool g_bootProfileSyncActive = false;

// ============================================================
// Pipeline state machine
// ============================================================
enum class Step
{
    STEP_DECIDE = 0,
    STEP_NET_ATTACH,
    STEP_MQTT_CONNECT,

    // Efter MQTT-connect väntar vi kort på retained desired_profile.
    STEP_BOOT_PROFILE_SYNC,

    STEP_PUBLISH,
    STEP_RX_DOWNLINK,
    STEP_CONNECTED_WAIT,
    STEP_MQTT_DISCONNECT,
    STEP_RF_OFF,
    STEP_IDLE_WAIT
};

static Step g_step = Step::STEP_DECIDE;
static uint32_t g_deadlineMs = 0;

// ============================================================
// PIR interrupt-läge från config
// ------------------------------------------------------------
// Tidigare var attachInterrupt hårdkodat till RISING.
// Nu låter vi PIR_RISING_EDGE i config.h styra beteendet på riktigt.
// ============================================================
static constexpr int PIR_INTERRUPT_MODE = PIR_RISING_EDGE ? RISING : FALLING;

// ============================================================
// ISR handlers
// ============================================================
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

// ============================================================
// Helpers
// ============================================================

// Robust tidsjämförelse för millis() med wraparound-stöd.
static inline bool timeReached(uint32_t nowMs, uint32_t targetMs)
{
    return (int32_t)(nowMs - targetMs) >= 0;
}

// Returnerar true om nowMs ligger före untilMs.
static inline bool isBefore(uint32_t nowMs, uint32_t untilMs)
{
    return (int32_t)(nowMs - untilMs) < 0;
}

// Returnerar true om aktuell profil har någon PIR aktiv.
static bool currentProfileUsesPir()
{
    const auto &p = currentProfile();
    return p.pirFront || p.pirBack;
}

// Returnerar true om systemet i detta ögonblick ska hålla RF/data/MQTT uppe.
static bool shouldKeepConnectedNow()
{
    return currentProfile().keepConnected;
}

// Om profilbyte väntar på att publiceras ska vi tillfälligt hålla
// anslutningen uppe även om nya profilen normalt inte är keepConnected.
static bool shouldHoldConnectionForProfilePublish()
{
    return g_profileChangePublishPending;
}

// Nollställ PIR-outbox.
static void pirClearOutbox()
{
    g_pir.pending = false;
    g_pir.event_id = 0;
    g_pir.count = 0;
    g_pir.first_ms = 0;
    g_pir.last_ms = 0;
    g_pir.src_mask = 0;
}

// Förläng timeouten för TRIGGERED.
static void triggeredExtendTimeout(uint32_t nowMs)
{
    if (currentProfile().id != ProfileId::TRIGGERED)
        return;

    if (currentProfile().autoReturnMs == 0)
        return;

    g_triggeredUntilMs = nowMs + currentProfile().autoReturnMs;
}

// Returnerar true om pending PIR får publiceras just nu.
// Regler:
// - Det måste finnas pending event
// - Minst en av berörda sensorer måste vara utanför throttle-fönstret
static bool pirCanPublishNow(uint32_t nowMs)
{
    if (!g_pir.pending)
        return false;

    bool allow = false;

    if (g_pir.src_mask & 0x01)
    {
        if (g_lastPirPublishFrontMs == 0 ||
            (int32_t)(nowMs - g_lastPirPublishFrontMs) >= (int32_t)PIR_THROTTLE_MS)
        {
            allow = true;
        }
    }

    if (g_pir.src_mask & 0x02)
    {
        if (g_lastPirPublishBackMs == 0 ||
            (int32_t)(nowMs - g_lastPirPublishBackMs) >= (int32_t)PIR_THROTTLE_MS)
        {
            allow = true;
        }
    }

    return allow;
}

// Markera att PIR har publicerats och sätt lockout.
// Viktigt:
// Detta ska bara anropas EFTER att mqttPublishPirEvent() lyckats.
// Annars riskerar vi att låsa ut retries fast publiceringen misslyckades.
static void pirMarkPublishedAndLockout(uint32_t nowMs)
{
    if (!g_pir.pending)
        return;

    if (g_pir.src_mask & 0x01)
    {
        g_lastPirPublishFrontMs = nowMs;
        g_pirIgnoreFrontUntilMs = nowMs + PIR_LOCKOUT_MS;
    }

    if (g_pir.src_mask & 0x02)
    {
        g_lastPirPublishBackMs = nowMs;
        g_pirIgnoreBackUntilMs = nowMs + PIR_LOCKOUT_MS;
    }
}

// Bygg en ExtGnssFix från senaste externa GNSS-fix.
// Returnerar true om giltig extern fix fanns.
static bool buildGpsFromExternal(ExtGnssFix &out)
{
    out = ExtGnssFix{};

#if EXTERNAL_GNSS_ENABLED
    return extGnssGetLatest(out);
#else
    return false;
#endif
}

// Gå in i nytt step och sätt eventuell timeout/omedelbar åtgärd.
static void stepEnter(Step s, uint32_t nowMs)
{
    g_step = s;

    switch (s)
    {
    case Step::STEP_DECIDE:
        break;

    case Step::STEP_NET_ATTACH:
        g_deadlineMs = nowMs + 60000UL;
        break;

    case Step::STEP_MQTT_CONNECT:
        g_deadlineMs = nowMs + 15000UL;
        break;

    case Step::STEP_BOOT_PROFILE_SYNC:
        // Precis efter connect ger vi retained desired profile
        // en kort chans att spelas upp innan första publish.
        g_bootProfileSyncActive = true;
        g_deadlineMs = nowMs + MQTT_BOOT_PROFILE_SYNC_WINDOW_MS;
        break;

    case Step::STEP_PUBLISH:
        // Så fort vi ska publicera är boot-sync-fönstret över.
        g_bootProfileSyncActive = false;
        g_deadlineMs = nowMs + 8000UL;
        break;

    case Step::STEP_RX_DOWNLINK:
        g_deadlineMs = nowMs + 5000UL;
        break;

    case Step::STEP_CONNECTED_WAIT:
        g_bootProfileSyncActive = false;
        g_deadlineMs = 0;
        break;

    case Step::STEP_MQTT_DISCONNECT:
        g_bootProfileSyncActive = false;
        mqttDisconnect();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_RF_OFF:
        g_bootProfileSyncActive = false;
        modemRfOff();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_IDLE_WAIT:
        g_bootProfileSyncActive = false;
        g_deadlineMs = g_nextCommAtMs;
        break;
    }
}

// Kontroll om aktuellt step nått deadline.
static bool stepTimedOut(uint32_t nowMs)
{
    return timeReached(nowMs, g_deadlineMs);
}

// ============================================================
// PIR ingest
// ------------------------------------------------------------
// Här läser vi ut ISR-state och gör all filtrering i vanlig kod.
// ============================================================
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

    if (n == 0 || mask == 0)
        return;

    if (!currentProfileUsesPir())
        return;

    const auto &p = currentProfile();

    if (!p.pirFront)
        mask &= ~0x01;
    if (!p.pirBack)
        mask &= ~0x02;

    if (mask == 0)
        return;

    uint8_t lockoutFiltered = mask;

    if ((lockoutFiltered & 0x01) && isBefore(nowMs, g_pirIgnoreFrontUntilMs))
        lockoutFiltered &= ~0x01;

    if ((lockoutFiltered & 0x02) && isBefore(nowMs, g_pirIgnoreBackUntilMs))
        lockoutFiltered &= ~0x02;

    if (lockoutFiltered == 0)
        return;

    uint8_t acceptedMask = 0;

    if (lockoutFiltered & 0x01)
    {
        if (g_lastPirAcceptedFrontMs == 0 ||
            (int32_t)(nowMs - g_lastPirAcceptedFrontMs) >= (int32_t)PIR_ACCEPT_MIN_GAP_MS)
        {
            acceptedMask |= 0x01;
            g_lastPirAcceptedFrontMs = nowMs;
        }
    }

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
        logSystemf("PIR: FILTERED (1Hz) raw_n=%u raw_mask=0x%02X",
                   (unsigned)n, (unsigned)mask);
        return;
    }

    const char *which = (acceptedMask == 0x01)   ? "FRONT"
                        : (acceptedMask == 0x02) ? "BACK"
                        : (acceptedMask == 0x03) ? "FRONT+BACK"
                                                 : "UNKNOWN";

    logSystemf("PIR: ACCEPTED which=%s raw_n=%u raw_mask=0x%02X accepted_mask=0x%02X",
               which, (unsigned)n, (unsigned)mask, (unsigned)acceptedMask);

    if (!g_pir.pending)
    {
        g_pir.pending = true;
        g_pir.event_id = g_nextEventId++;
        g_pir.count = 0;
        g_pir.first_ms = nowMs;
        g_pir.last_ms = nowMs;
        g_pir.src_mask = 0;
    }

    uint16_t add = 0;
    if (acceptedMask & 0x01)
        add++;
    if (acceptedMask & 0x02)
        add++;

    g_pir.count = (uint16_t)(g_pir.count + add);
    g_pir.last_ms = nowMs;
    g_pir.src_mask |= acceptedMask;

    if (p.id == ProfileId::ARMED)
    {
        logSystem("PIR: auto profile change ARMED -> TRIGGERED");
        setProfile(ProfileId::TRIGGERED);
        triggeredExtendTimeout(nowMs);
    }
    else if (p.id == ProfileId::TRIGGERED)
    {
        triggeredExtendTimeout(nowMs);
    }
}

// ============================================================
// Hooks från andra moduler
// ============================================================
void pipelineOnPirAck(uint32_t eventId)
{
    if (g_pir.pending && g_pir.event_id == eventId)
    {
        logSystemf("PIR: ACK -> clear outbox event_id=%u", (unsigned)eventId);
        pirClearOutbox();
    }
}

// Anropas när profil ändras.
void pipelineOnProfileChanged(ProfileId newProfile)
{
    uint32_t nowMs = millis();

    // --------------------------------------------------------
    // TRIGGERED: sätt timeout för auto-return till ARMED
    // --------------------------------------------------------
    if (newProfile == ProfileId::TRIGGERED)
    {
        if (currentProfile().autoReturnMs > 0)
        {
            g_triggeredUntilMs = nowMs + currentProfile().autoReturnMs;
        }
        else
        {
            g_triggeredUntilMs = 0;
        }
    }
    else
    {
        g_triggeredUntilMs = 0;
    }

    // --------------------------------------------------------
    // När vi lämnar PIR-familjen helt kan vi rensa PIR-state.
    // --------------------------------------------------------
    if (newProfile == ProfileId::PARKED || newProfile == ProfileId::TRAVEL)
    {
        pirClearOutbox();
        g_lastPirAcceptedFrontMs = 0;
        g_lastPirAcceptedBackMs = 0;
    }

    // --------------------------------------------------------
    // Planera om nästa ordinarie kommunikationsfönster enligt
    // nya profilen.
    // --------------------------------------------------------
    g_nextCommAtMs = nowMs + currentProfile().commIntervalMs;

    // --------------------------------------------------------
    // Om vi redan är MQTT-anslutna när profil ändras ska vi ge
    // systemet chans att publicera minst en gång med nya profilen
    // innan disconnect tillåts.
    // --------------------------------------------------------
    if (mqttIsConnected())
    {
        g_profileChangePublishPending = true;
    }

    // --------------------------------------------------------
    // Specialfall:
    // Om profil ändras medan vi fortfarande är i första sync-fönstret
    // efter MQTT connect, ska vi INTE gå direkt till disconnect bara
    // för att den nya profilen inte är keepConnected.
    //
    // I stället går vi till publish så att första publiceringen sker
    // med rätt profil.
    // --------------------------------------------------------
    if (g_bootProfileSyncActive && mqttIsConnected())
    {
        logSystem("PIPELINE: profile changed during boot sync -> publish with new profile");
        stepEnter(Step::STEP_PUBLISH, nowMs);
        return;
    }

    // --------------------------------------------------------
    // Om vi är anslutna och har pending profilpublish ska vi
    // gå till publish direkt istället för att disconnecta.
    // --------------------------------------------------------
    if (mqttIsConnected() && g_profileChangePublishPending)
    {
        logSystem("PIPELINE: profile changed -> publish pending before disconnect");
        stepEnter(Step::STEP_PUBLISH, nowMs);
        return;
    }

    // --------------------------------------------------------
    // Normal styrning av state machine efter profilbyte.
    // --------------------------------------------------------
    if (shouldKeepConnectedNow() && mqttIsConnected())
    {
        stepEnter(Step::STEP_CONNECTED_WAIT, nowMs);
    }
    else if (!shouldKeepConnectedNow() && mqttIsConnected())
    {
        stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
    }
    else
    {
        stepEnter(Step::STEP_DECIDE, nowMs);
    }
}

// ============================================================
// Init
// ============================================================
void pipelineInit()
{
#if EXTERNAL_GNSS_ENABLED
    extGnssBegin(PIN_GNSS_RX, PIN_GNSS_TX, GNSS_BAUD);
    logSystem("GPS: using external GNSS (UART)");
#endif

    // Första kommunikationsförsök en liten stund efter boot
    g_nextCommAtMs = millis() + 2000UL;

    // Om defaultprofil inte ska hålla anslutning uppe:
    // stäng RF direkt initialt.
    if (!shouldKeepConnectedNow())
    {
        modemRfOff();
    }

    // PIR-ingångar
    pinMode(PIN_PIR_FRONT, INPUT_PULLDOWN);
    pinMode(PIN_PIR_BACK, INPUT_PULLDOWN);

    // Viktigt:
    // Interrupt-läge styrs nu av PIR_RISING_EDGE i config.h.
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_FRONT), isrPirFront, PIR_INTERRUPT_MODE);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_BACK), isrPirBack, PIR_INTERRUPT_MODE);

    stepEnter(Step::STEP_DECIDE, millis());
}

// ============================================================
// Huvudtick
// ============================================================
void pipelineTick(uint32_t nowMs)
{
#if EXTERNAL_GNSS_ENABLED
    // Poll extern GNSS kontinuerligt så senaste fix hålls uppdaterad
    extGnssPoll();
#endif

    // Hämta in PIR-data från ISR varje tick
    pirIngestIsr(nowMs);

    // Automatisk TRIGGERED -> ARMED när timeout går ut
    if (currentProfile().id == ProfileId::TRIGGERED &&
        currentProfile().autoReturnMs > 0 &&
        g_triggeredUntilMs > 0 &&
        timeReached(nowMs, g_triggeredUntilMs))
    {
        logSystem("PROFILE: auto return TRIGGERED -> ARMED");
        setProfile(ProfileId::ARMED);
    }

    switch (g_step)
    {
    case Step::STEP_DECIDE:
    {
        bool commDue = timeReached(nowMs, g_nextCommAtMs);
        bool needComm = g_pir.pending || commDue;

        if (needComm)
        {
            if (mqttIsConnected())
            {
                stepEnter(Step::STEP_PUBLISH, nowMs);
            }
            else
            {
                stepEnter(Step::STEP_NET_ATTACH, nowMs);
            }
            break;
        }

        if ((shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()) && mqttIsConnected())
        {
            stepEnter(Step::STEP_CONNECTED_WAIT, nowMs);
        }
        else if (!(shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()) && mqttIsConnected())
        {
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
        }
        else if (!shouldKeepConnectedNow())
        {
            stepEnter(Step::STEP_IDLE_WAIT, nowMs);
        }
        else
        {
            stepEnter(Step::STEP_IDLE_WAIT, nowMs);
        }

        break;
    }

    case Step::STEP_NET_ATTACH:
    {
        // OBS: modemConnectData() är fortfarande blockerande.
        NetResult net;
        bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);

        if (ok)
        {
            // Försök först tid från modemet, sedan NTP
            timeSyncFromModem();
            timeSyncFromNtp(8000);

            stepEnter(Step::STEP_MQTT_CONNECT, nowMs);
            break;
        }

        // Misslyckad attach -> försök igen nästa intervall
        g_nextCommAtMs = nowMs + currentProfile().commIntervalMs;

        if (shouldKeepConnectedNow())
        {
            stepEnter(Step::STEP_IDLE_WAIT, nowMs);
        }
        else
        {
            stepEnter(Step::STEP_RF_OFF, nowMs);
        }

        break;
    }

    case Step::STEP_MQTT_CONNECT:
        if (mqttConnect())
        {
            // Viktigt:
            // efter connect går vi INTE direkt till publish,
            // utan ger retained desired_profile en chans först.
            stepEnter(Step::STEP_BOOT_PROFILE_SYNC, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            g_nextCommAtMs = nowMs + currentProfile().commIntervalMs;

            if (shouldKeepConnectedNow())
            {
                stepEnter(Step::STEP_IDLE_WAIT, nowMs);
            }
            else
            {
                stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            }
        }
        break;

    case Step::STEP_BOOT_PROFILE_SYNC:
        // Under detta korta fönster kör vi mqttLoop så att retained
        // desired_profile faktiskt hinner processas av callbacken.
        mqttLoop();

        // Om retained profil redan hunnit komma kan vi gå vidare direkt.
        if (mqttHasSeenDesiredProfileThisConnect())
        {
            logSystem("PIPELINE: desired profile received during boot sync");
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

        // Om inget kom inom sync-fönstret fortsätter vi ändå.
        // Då publicerar vi med nuvarande profil.
        if (stepTimedOut(nowMs))
        {
            logSystem("PIPELINE: boot profile sync timeout -> continue");
            stepEnter(Step::STEP_PUBLISH, nowMs);
        }
        break;

    case Step::STEP_PUBLISH:
    {
        bool ackOk = mqttPublishPendingProfileAck();

        ExtGnssFix fx;
        bool fixOk = buildGpsFromExternal(fx);
        bool gpsOk = mqttPublishGpsSingle(fx, fixOk);
        (void)gpsOk; // ännu inte använd i styrlogiken

        bool pirOk = true;
        if (g_pir.pending)
        {
            if (pirCanPublishNow(nowMs))
            {
                // Viktig ändring:
                // publicera först, sätt publish-timestamp/lockout efter lyckad publish.
                pirOk = mqttPublishPirEvent(g_pir.event_id,
                                            g_pir.count,
                                            g_pir.first_ms,
                                            g_pir.last_ms,
                                            g_pir.src_mask);

                if (pirOk)
                {
                    pirMarkPublishedAndLockout(nowMs);
                }
                else
                {
                    logSystem("PIR: publish failed -> keep pending, no lockout");
                }
            }
            else
            {
                logSystem("PIR: blocked by 1/min rule -> drop");
                pirClearOutbox();
            }
        }

        bool aliveOk = mqttPublishAlive();
        (void)pirOk; // ännu inte använd i styrlogiken

        // Profilbyte räknas som klart först när:
        // - pending ACK är publicerad eller ingen ACK väntar
        // - alive med aktuell profil har publicerats OK
        if (g_profileChangePublishPending)
        {
            if (ackOk && aliveOk && !mqttHasPendingProfileAck())
            {
                logSystem("PIPELINE: profile change publish completed");
                g_profileChangePublishPending = false;
            }
            else
            {
                logSystem("PIPELINE: profile change publish still pending");
            }
        }

        // Planera nästa ordinarie kommunikation
        g_nextCommAtMs = nowMs + currentProfile().commIntervalMs;

        if (shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish())
        {
            stepEnter(Step::STEP_CONNECTED_WAIT, nowMs);
        }
        else
        {
            stepEnter(Step::STEP_RX_DOWNLINK, nowMs);
        }

        break;
    }

    case Step::STEP_RX_DOWNLINK:
        mqttLoop();

        if (stepTimedOut(nowMs))
        {
            if (shouldKeepConnectedNow())
            {
                stepEnter(Step::STEP_CONNECTED_WAIT, nowMs);
            }
            else
            {
                stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            }
        }
        break;

    case Step::STEP_CONNECTED_WAIT:
        if (!mqttIsConnected())
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }

        mqttLoop();

        if (g_pir.pending && pirCanPublishNow(nowMs))
        {
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

        if (timeReached(nowMs, g_nextCommAtMs))
        {
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

        if (!(shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()))
        {
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            break;
        }

        break;

    case Step::STEP_MQTT_DISCONNECT:
        if (shouldHoldConnectionForProfilePublish())
        {
            logSystem("PIPELINE: disconnect blocked, waiting for profile publish");
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            if (shouldKeepConnectedNow())
            {
                stepEnter(Step::STEP_IDLE_WAIT, nowMs);
            }
            else
            {
                stepEnter(Step::STEP_RF_OFF, nowMs);
            }
        }
        break;

    case Step::STEP_RF_OFF:
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_IDLE_WAIT, nowMs);
        }
        break;

    case Step::STEP_IDLE_WAIT:
        if (g_pir.pending || timeReached(nowMs, g_nextCommAtMs))
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }

        if ((shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()) && mqttIsConnected())
        {
            stepEnter(Step::STEP_CONNECTED_WAIT, nowMs);
            break;
        }

        if (!(shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()) && mqttIsConnected())
        {
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            break;
        }

        break;

    default:
        stepEnter(Step::STEP_DECIDE, nowMs);
        break;
    }
}