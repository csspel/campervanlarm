#include "pipeline.h"

#include "config.h"
#include "ext_gnss.h"
#include "logging.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "time_manager.h"
#include "victron_manager.h"

#include <WiFi.h>

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
// Activity boost från PIR fram
// ------------------------------------------------------------
// PIR fram kan ge falska utslag när någon går utanför bilen.
// Därför används PIR fram i PARKED/ARMED som tyst aktivitetstrigger:
// - ingen larmnotis
// - ingen PIR-outbox till HA
// - ingen automatisk TRIGGERED
// - tillfälligt tätare GPS/MQTT-kommunikation
// ============================================================
struct ActivityBoostState
{
    bool active = false;
    bool activityEventPending = false;
    bool endEventPending = false;
    uint32_t startedMs = 0;
    uint32_t untilMs = 0;
    uint32_t lastTriggerMs = 0;
    uint32_t lockoutUntilMs = 0;
    uint8_t falseBoostCount = 0;
    bool realEventSeen = false;
    const char *reason = "NONE";
};

static ActivityBoostState g_activity;


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
    STEP_IDLE_WAIT,

    // Kort, kontrollerad BLE-scan av Victron-enheter.
    STEP_VICTRON_BLE_SCAN,

    // Nytt: kontrollerat återhämtningsläge.
    STEP_RECOVERY_WAIT
};

static Step g_step = Step::STEP_DECIDE;
static uint32_t g_deadlineMs = 0;

// ============================================================
// Enkel recovery-manager
// ------------------------------------------------------------
// Första steget här är medvetet enkelt:
// - vid enstaka fel: prova om senare
// - vid flera fel i rad: stäng MQTT / RF och börja om
// - vid ännu fler fel: power-cycle modemet
//
// Detta är inte hela målbilden, men ett tydligt steg upp i robusthet.
// ============================================================
enum class RecoveryReason
{
    NONE = 0,
    NET_ATTACH_FAILED,
    MQTT_CONNECT_TIMEOUT,
    MQTT_DROPPED,
    PUBLISH_FAILED,
    NO_PROGRESS,
    STEP_TIMEOUT
};

enum class RecoveryAction
{
    NONE = 0,
    RETRY_LATER,
    RESTART_LINK,
    POWER_CYCLE_MODEM
};

struct RecoveryState
{
    RecoveryReason reason = RecoveryReason::NONE;
    RecoveryAction action = RecoveryAction::NONE;
    uint8_t consecutiveFailures = 0;
    uint32_t executeAtMs = 0;
};

static RecoveryState g_recovery;

// ============================================================
// Health / driftövervakning
// ------------------------------------------------------------
// Dessa värden publiceras på separat health-topic så att Node-RED,
// InfluxDB och Grafana kan övervaka stabilitet över tid.
// ============================================================
static uint32_t g_recoveryCountBoot = 0;
static uint32_t g_netConnectCountBoot = 0;
static uint32_t g_mqttConnectCountBoot = 0;
static uint32_t g_lastNetConnectMs = 0;
static RecoveryReason g_lastRecoveryReason = RecoveryReason::NONE;

// ============================================================
// Network link selection – steg 3
// ------------------------------------------------------------
// Här väljer pipeline vilken länk som ska provas först och om backup
// får användas. MQTT är målet: fallback triggas både av länkfel
// och av MQTT-connect-fel.
//
// WIFI_ONLY    -> endast WiFi
// SIM_ONLY     -> endast SIM
// WIFI_PRIMARY -> WiFi först, SIM som backup
// SIM_PRIMARY  -> SIM först, WiFi som backup
// AUTO         -> WiFi först, SIM som backup tills vidare.
// ============================================================
enum class NetAttemptLink
{
    NONE = 0,
    SIM,
    WIFI
};

static NetAttemptLink g_netAttemptLink = NetAttemptLink::NONE;
static NetAttemptLink g_forcedNextNetAttemptLink = NetAttemptLink::NONE;
static bool g_netAttemptStarted = false;
static uint32_t g_netAttemptStartedMs = 0;
static bool g_netFallbackTried = false;
static String g_lastFallbackReason = "NONE";

static bool modeWantsWifiFirst(const char *mode)
{
    if (!mode)
        return false;

    String m(mode);
    m.toUpperCase();

    return m == "WIFI_ONLY" || m == "WIFI_PRIMARY" || m == "AUTO";
}


static NetAttemptLink primaryLinkForMode(const char *mode)
{
    if (!mode)
        return NetAttemptLink::SIM;

    String m(mode);
    m.toUpperCase();

    if (m == "WIFI_ONLY" || m == "WIFI_PRIMARY" || m == "AUTO")
        return NetAttemptLink::WIFI;

    return NetAttemptLink::SIM;
}

static NetAttemptLink fallbackLinkForMode(const char *mode, NetAttemptLink failedLink)
{
    if (!mode)
        return NetAttemptLink::NONE;

    String m(mode);
    m.toUpperCase();

    if ((m == "WIFI_PRIMARY" || m == "AUTO") && failedLink == NetAttemptLink::WIFI)
        return NetAttemptLink::SIM;

    if (m == "SIM_PRIMARY" && failedLink == NetAttemptLink::SIM)
        return NetAttemptLink::WIFI;

    return NetAttemptLink::NONE;
}

static const char *netAttemptLinkName(NetAttemptLink link)
{
    switch (link)
    {
    case NetAttemptLink::SIM:
        return "SIM";
    case NetAttemptLink::WIFI:
        return "WIFI";
    default:
        return "NONE";
    }
}

static void wifiPowerOff()
{
    if (WiFi.getMode() != WIFI_OFF)
    {
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        logSystem("WIFI: off");
    }
}

static void startWifiConnect(uint32_t nowMs)
{
    g_netAttemptStarted = true;
    g_netAttemptStartedMs = nowMs;

    mqttUseWifiClient();
    mqttSetNetStatus("WIFI", false, false, false, 0, -1, "WIFI_CONNECTING");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);

    logSystem(String("WIFI: connecting ssid=") + WIFI_AP_SSID);
}

static bool tickWifiConnect(uint32_t nowMs, bool &ok)
{
    ok = false;

    if (!g_netAttemptStarted)
    {
        startWifiConnect(nowMs);
        return false;
    }

    wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED)
    {
        int rssi = WiFi.RSSI();
        String ip = WiFi.localIP().toString();

        mqttUseWifiClient();
        mqttSetNetStatus("WIFI", true, false, false, rssi, -1, "NONE");

        logSystem("WIFI: connected OK ip=" + ip + " rssi=" + String(rssi));
        ok = true;
        return true;
    }

    if ((uint32_t)(nowMs - g_netAttemptStartedMs) >= WIFI_CONNECT_TIMEOUT_MS)
    {
        mqttSetNetStatus("NONE", false, false, false, 0, -1, "WIFI_TIMEOUT");
        logSystem("WIFI: connect timeout");
        wifiPowerOff();
        ok = false;
        return true;
    }

    return false;
}


static void requestRecovery(RecoveryReason reason, uint32_t nowMs);
static void stepEnter(Step s, uint32_t nowMs);

static void cleanupNetAttemptLink(NetAttemptLink link)
{
    if (link == NetAttemptLink::WIFI)
    {
        wifiPowerOff();
    }
    else if (link == NetAttemptLink::SIM)
    {
        modemAbortConnectData();
        modemRfOff();
    }
}

static bool tryFallbackOrRecovery(RecoveryReason reason, const char *failReason, uint32_t nowMs)
{
    NetAttemptLink fallback = fallbackLinkForMode(mqttGetDesiredNetMode(), g_netAttemptLink);

    if (!g_netFallbackTried && fallback != NetAttemptLink::NONE)
    {
        const char *fromName = netAttemptLinkName(g_netAttemptLink);
        const char *toName = netAttemptLinkName(fallback);

        g_netFallbackTried = true;
        g_lastFallbackReason = String(failReason && failReason[0] ? failReason : "PRIMARY_FAILED") +
                               "_FALLBACK_TO_" + String(toName);

        logSystem(String("PIPELINE: fallback ") + fromName + " -> " + toName +
                  " reason=" + g_lastFallbackReason);

        mqttDisconnect();
        cleanupNetAttemptLink(g_netAttemptLink);

        mqttSetNetStatus("NONE", false, false, false, 0, -1, g_lastFallbackReason.c_str());

        g_forcedNextNetAttemptLink = fallback;
        stepEnter(Step::STEP_NET_ATTACH, nowMs);
        return true;
    }

    requestRecovery(reason, nowMs);
    return false;
}

// ============================================================
// Progress-spårning
// ------------------------------------------------------------
// Hjälper oss att se om systemet gör verkliga framsteg.
// ============================================================
static uint32_t g_lastProgressMs = 0;
static uint32_t g_lastSuccessfulMqttConnectMs = 0;
static uint32_t g_lastSuccessfulPublishMs = 0;

// När vi förväntar oss framsteg i keepConnected-lägen.
// Detta används för att upptäcka "halvdött" läge där MQTT ser
// levande ut men inga riktiga publiceringar lyckas längre.
static const uint32_t PIPE_NO_PROGRESS_LIMIT_MS = 5UL * 60UL * 1000UL;

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

#if FRONT_PIR_ACTIVITY_ENABLED
    // PARKED/ARMED/ALARM ska kunna använda PIR fram som tyst activity-trigger
    // även om den inte används som skarp larmsensor.
    if (p.id == ProfileId::PARKED || p.id == ProfileId::ARMED || p.id == ProfileId::ALARM)
        return true;
#endif

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

static bool activityBoostAllowedInProfile(ProfileId id)
{
#if FRONT_PIR_ACTIVITY_ENABLED
    return id == ProfileId::PARKED ||
           id == ProfileId::ARMED ||
           id == ProfileId::ALARM;
#else
    (void)id;
    return false;
#endif
}

static bool activityBoostActive(uint32_t nowMs)
{
#if FRONT_PIR_ACTIVITY_ENABLED
    if (!g_activity.active)
        return false;

    if (timeReached(nowMs, g_activity.untilMs))
        return false;

    return true;
#else
    (void)nowMs;
    return false;
#endif
}

static uint32_t currentCommIntervalMs(uint32_t nowMs)
{
    if (activityBoostActive(nowMs))
        return ACTIVITY_BOOST_COMM_INTERVAL_MS;

    return currentProfile().commIntervalMs;
}

static void activityBoostStartOrExtend(uint32_t nowMs, const char *reason)
{
#if FRONT_PIR_ACTIVITY_ENABLED
    if (!activityBoostAllowedInProfile(currentProfile().id))
        return;

    if (isBefore(nowMs, g_activity.lockoutUntilMs))
    {
        logSystem("ACTIVITY: PIR front ignored by activity lockout");
        return;
    }

    if (g_activity.lastTriggerMs != 0 &&
        (uint32_t)(nowMs - g_activity.lastTriggerMs) < ACTIVITY_BOOST_RETRIGGER_GAP_MS)
    {
        logSystem("ACTIVITY: PIR front ignored by retrigger gap");
        return;
    }

    g_activity.lastTriggerMs = nowMs;

    if (!g_activity.active || timeReached(nowMs, g_activity.untilMs))
    {
        g_activity.active = true;
        g_activity.startedMs = nowMs;
        g_activity.untilMs = nowMs + ACTIVITY_BOOST_DURATION_MS;
        g_activity.reason = reason ? reason : "PIR_FRONT";
        g_activity.realEventSeen = false;
        g_activity.activityEventPending = true;
        g_activity.endEventPending = false;

        logSystemf("ACTIVITY: boost started reason=%s duration_s=%lu",
                   g_activity.reason,
                   (unsigned long)(ACTIVITY_BOOST_DURATION_MS / 1000UL));
    }
    else
    {
        uint32_t maxUntil = g_activity.startedMs + ACTIVITY_BOOST_MAX_CONTINUOUS_MS;
        uint32_t wantedUntil = nowMs + ACTIVITY_BOOST_DURATION_MS;

        g_activity.untilMs = isBefore(wantedUntil, maxUntil) ? wantedUntil : maxUntil;
        g_activity.activityEventPending = true;

        logSystemf("ACTIVITY: boost extended reason=%s until_ms=%lu",
                   g_activity.reason,
                   (unsigned long)g_activity.untilMs);
    }

    // Starta kommunikation direkt så första tätare GPS/MQTT-cykeln inte väntar.
    g_nextCommAtMs = nowMs;
#else
    (void)nowMs;
    (void)reason;
#endif
}

static void activityBoostTick(uint32_t nowMs)
{
#if FRONT_PIR_ACTIVITY_ENABLED
    if (!g_activity.active)
        return;

    if (!timeReached(nowMs, g_activity.untilMs))
        return;

    g_activity.active = false;
    g_activity.endEventPending = true;

    // Om boost tog slut utan att något annat hanterade profilen, räkna den som "falsk/tyst".
    // Detta är ett rent strömskydd för platser där folk går förbi ofta.
    if (!g_activity.realEventSeen)
    {
        g_activity.falseBoostCount++;
        if (g_activity.falseBoostCount >= ACTIVITY_BOOST_FALSE_LIMIT)
        {
            g_activity.lockoutUntilMs = nowMs + ACTIVITY_BOOST_FALSE_LOCKOUT_MS;
            g_activity.falseBoostCount = 0;
            logSystem("ACTIVITY: false boost limit reached -> front PIR activity lockout");
        }
    }

    logSystem("ACTIVITY: boost ended");
#else
    (void)nowMs;
#endif
}

static bool activityBoostHasPendingMqttEvent()
{
#if FRONT_PIR_ACTIVITY_ENABLED
    return g_activity.activityEventPending || g_activity.endEventPending;
#else
    return false;
#endif
}

static void activityBoostMarkRealEvent()
{
#if FRONT_PIR_ACTIVITY_ENABLED
    // När något annat faktiskt händer, t.ex. PIR bak/larm/profilbyte, ska inte
    // tidigare front-boost räknas som falsk.
    g_activity.falseBoostCount = 0;
    if (g_activity.active)
        g_activity.realEventSeen = true;
#else
#endif
}

static void activityBoostCancel()
{
#if FRONT_PIR_ACTIVITY_ENABLED
    g_activity.active = false;
    g_activity.activityEventPending = false;
    g_activity.endEventPending = false;
    g_activity.realEventSeen = false;
#else
#endif
}

static bool activityBoostPublishPending()
{
#if FRONT_PIR_ACTIVITY_ENABLED
    if (g_activity.activityEventPending)
    {
        bool ok = mqttPublishActivityEvent(g_activity.reason,
                                           true,
                                           ACTIVITY_BOOST_DURATION_MS / 1000UL,
                                           g_activity.untilMs,
                                           "front_pir_activity_boost");
        if (ok)
            g_activity.activityEventPending = false;
        return ok;
    }

    if (g_activity.endEventPending)
    {
        bool ok = mqttPublishActivityEvent(g_activity.reason,
                                           false,
                                           0,
                                           g_activity.untilMs,
                                           "activity_boost_ended");
        if (ok)
            g_activity.endEventPending = false;
        return ok;
    }

    return true;
#else
    return true;
#endif
}

// Namn för step i loggar.
static const char *stepName(Step s)
{
    switch (s)
    {
    case Step::STEP_DECIDE:
        return "DECIDE";
    case Step::STEP_NET_ATTACH:
        return "NET_ATTACH";
    case Step::STEP_MQTT_CONNECT:
        return "MQTT_CONNECT";
    case Step::STEP_BOOT_PROFILE_SYNC:
        return "BOOT_PROFILE_SYNC";
    case Step::STEP_PUBLISH:
        return "PUBLISH";
    case Step::STEP_RX_DOWNLINK:
        return "RX_DOWNLINK";
    case Step::STEP_CONNECTED_WAIT:
        return "CONNECTED_WAIT";
    case Step::STEP_MQTT_DISCONNECT:
        return "MQTT_DISCONNECT";
    case Step::STEP_RF_OFF:
        return "RF_OFF";
    case Step::STEP_IDLE_WAIT:
        return "IDLE_WAIT";
    case Step::STEP_VICTRON_BLE_SCAN:
        return "VICTRON_BLE_SCAN";
    case Step::STEP_RECOVERY_WAIT:
        return "RECOVERY_WAIT";
    default:
        return "UNKNOWN";
    }
}

static const char *recoveryReasonName(RecoveryReason r)
{
    switch (r)
    {
    case RecoveryReason::NONE:
        return "NONE";
    case RecoveryReason::NET_ATTACH_FAILED:
        return "NET_ATTACH_FAILED";
    case RecoveryReason::MQTT_CONNECT_TIMEOUT:
        return "MQTT_CONNECT_TIMEOUT";
    case RecoveryReason::MQTT_DROPPED:
        return "MQTT_DROPPED";
    case RecoveryReason::PUBLISH_FAILED:
        return "PUBLISH_FAILED";
    case RecoveryReason::NO_PROGRESS:
        return "NO_PROGRESS";
    case RecoveryReason::STEP_TIMEOUT:
        return "STEP_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

static const char *recoveryActionName(RecoveryAction a)
{
    switch (a)
    {
    case RecoveryAction::NONE:
        return "NONE";
    case RecoveryAction::RETRY_LATER:
        return "RETRY_LATER";
    case RecoveryAction::RESTART_LINK:
        return "RESTART_LINK";
    case RecoveryAction::POWER_CYCLE_MODEM:
        return "POWER_CYCLE_MODEM";
    default:
        return "UNKNOWN";
    }
}

// Enkel backoff för återförsök.
static uint32_t recoveryBackoffMs(uint8_t failures)
{
    if (failures <= 1)
        return 5000UL;
    if (failures == 2)
        return 15000UL;
    if (failures == 3)
        return 30000UL;
    if (failures == 4)
        return 60000UL;
    return 120000UL;
}

// Bestäm recovery-nivå utifrån antal fel i rad.
// Första fel: prova om senare.
// Sedan: starta om länk.
// Till sist: power-cycle modem.
static RecoveryAction chooseRecoveryAction(uint8_t consecutiveFailures)
{
    if (consecutiveFailures <= 1)
        return RecoveryAction::RETRY_LATER;
    if (consecutiveFailures <= 3)
        return RecoveryAction::RESTART_LINK;
    return RecoveryAction::POWER_CYCLE_MODEM;
}

// Markera att systemet gjort verkligt framsteg.
static void markProgress(uint32_t nowMs, const char *reason)
{
    g_lastProgressMs = nowMs;
    logSystemf("PIPELINE: progress -> %s", reason);
}

// Markera att systemet är friskt igen.
static void markHealthy(uint32_t nowMs, const char *reason)
{
    g_recovery.reason = RecoveryReason::NONE;
    g_recovery.action = RecoveryAction::NONE;
    g_recovery.consecutiveFailures = 0;
    g_recovery.executeAtMs = 0;

    markProgress(nowMs, reason);
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

// Begär recovery och gå till RECOVERY_WAIT.
static void requestRecovery(RecoveryReason reason, uint32_t nowMs)
{
    modemAbortConnectData();
    g_netAttemptStarted = false;
    g_netAttemptLink = NetAttemptLink::NONE;

    g_recovery.reason = reason;
    g_recovery.consecutiveFailures++;
    g_recovery.action = chooseRecoveryAction(g_recovery.consecutiveFailures);
    g_recovery.executeAtMs = nowMs + recoveryBackoffMs(g_recovery.consecutiveFailures);

    g_recoveryCountBoot++;
    g_lastRecoveryReason = reason;

    logSystemf("RECOVERY: requested reason=%s action=%s failures=%u backoff_ms=%lu",
               recoveryReasonName(g_recovery.reason),
               recoveryActionName(g_recovery.action),
               (unsigned)g_recovery.consecutiveFailures,
               (unsigned long)recoveryBackoffMs(g_recovery.consecutiveFailures));

    g_bootProfileSyncActive = false;
    g_profileChangePublishPending = false;
    g_deadlineMs = g_recovery.executeAtMs;
    g_step = Step::STEP_RECOVERY_WAIT;
}

// Utför recovery-åtgärd när backoff gått ut.
static void performRecovery(uint32_t nowMs)
{
    if (!timeReached(nowMs, g_recovery.executeAtMs))
        return;

    logSystemf("RECOVERY: execute reason=%s action=%s failures=%u",
               recoveryReasonName(g_recovery.reason),
               recoveryActionName(g_recovery.action),
               (unsigned)g_recovery.consecutiveFailures);

    // Städa alltid först.
    modemAbortConnectData();
    mqttDisconnect();

    if (String(mqttGetActiveLink()) == "WIFI")
    {
        wifiPowerOff();
    }

    switch (g_recovery.action)
    {
    case RecoveryAction::RETRY_LATER:
        // Ingen hård åtgärd, bara börja om kontrollerat.
        if (!shouldKeepConnectedNow())
        {
            modemRfOff();
        }
        g_nextCommAtMs = nowMs + currentProfile().commIntervalMs;
        g_step = Step::STEP_DECIDE;
        g_deadlineMs = 0;
        break;

    case RecoveryAction::RESTART_LINK:
        // Lättare återstart av kommunikationskedjan.
        modemRfOff();
        g_nextCommAtMs = nowMs + 2000UL;
        g_step = Step::STEP_DECIDE;
        g_deadlineMs = 0;
        break;

    case RecoveryAction::POWER_CYCLE_MODEM:
        // Tyngsta åtgärden som finns tillgänglig i nuvarande gränssnitt.
        modemPowerCycle();
        g_nextCommAtMs = nowMs + 3000UL;
        g_step = Step::STEP_DECIDE;
        g_deadlineMs = 0;
        break;

    default:
        g_step = Step::STEP_DECIDE;
        g_deadlineMs = 0;
        break;
    }
}

// Gå in i nytt step och sätt eventuell timeout/omedelbar åtgärd.
static void stepEnter(Step s, uint32_t nowMs)
{
    Step old = g_step;

    // Om vi lämnar NET_ATTACH ska ett ev. pågående attach-försök avbrytas.
    if (old == Step::STEP_NET_ATTACH && s != Step::STEP_NET_ATTACH)
    {
        modemAbortConnectData();
    }

    g_step = s;

    switch (s)
    {
    case Step::STEP_DECIDE:
        g_deadlineMs = 0;
        break;

    case Step::STEP_NET_ATTACH:
        g_netAttemptStarted = false;

        if (g_forcedNextNetAttemptLink != NetAttemptLink::NONE)
        {
            g_netAttemptLink = g_forcedNextNetAttemptLink;
            g_forcedNextNetAttemptLink = NetAttemptLink::NONE;
        }
        else
        {
            g_netAttemptLink = primaryLinkForMode(mqttGetDesiredNetMode());
            g_netFallbackTried = false;
            g_lastFallbackReason = "NONE";
        }

        if (g_netAttemptLink == NetAttemptLink::WIFI)
        {
            g_deadlineMs = nowMs + WIFI_CONNECT_TIMEOUT_MS + 5000UL;
        }
        else
        {
            // Extra pipeline-timeout som skyddsnät ovanpå modemets egen state machine.
            g_deadlineMs = nowMs + NET_REG_TIMEOUT_MS + DATA_ATTACH_TIMEOUT_MS + 30000UL;
        }

        logSystem(String("PIPELINE: NET_ATTACH target=") + netAttemptLinkName(g_netAttemptLink) +
                  " net_mode=" + mqttGetDesiredNetMode());
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
        if (String(mqttGetActiveLink()) == "WIFI")
        {
            wifiPowerOff();
        }
        else
        {
            modemRfOff();
        }
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_IDLE_WAIT:
        g_bootProfileSyncActive = false;
        g_deadlineMs = g_nextCommAtMs;
        break;

    case Step::STEP_VICTRON_BLE_SCAN:
        g_bootProfileSyncActive = false;
        // Scan körs blockande direkt i tick och går sedan tillbaka till DECIDE.
        g_deadlineMs = nowMs + (currentProfile().victronBleScanSeconds * 1000UL) + 5000UL;
        break;

    case Step::STEP_RECOVERY_WAIT:
        // requestRecovery() sätter deadline direkt.
        break;
    }

    if (old != s)
    {
        if (g_deadlineMs != 0)
        {
            logSystemf("PIPELINE: step %s -> %s deadline_in_ms=%lu",
                       stepName(old),
                       stepName(s),
                       (unsigned long)(g_deadlineMs - nowMs));
        }
        else
        {
            logSystemf("PIPELINE: step %s -> %s",
                       stepName(old),
                       stepName(s));
        }
    }
}

// Kontroll om aktuellt step nått deadline.
static bool stepTimedOut(uint32_t nowMs)
{
    if (g_deadlineMs == 0)
        return false;

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
    uint8_t rawMask;

    noInterrupts();
    n = g_pirIsrCount;
    rawMask = g_pirIsrMask;
    g_pirIsrCount = 0;
    g_pirIsrMask = 0;
    interrupts();

    if (n == 0 || rawMask == 0)
        return;

    if (!currentProfileUsesPir())
        return;

    const auto &p = currentProfile();
    uint8_t mask = rawMask;

    // PIR fram får användas som activity trigger i PARKED/ARMED/ALARM.
    // PIR bak följer profiltabellen.
    bool frontAllowed = p.pirFront ||
                        (FRONT_PIR_ACTIVITY_ENABLED &&
                         (p.id == ProfileId::PARKED || p.id == ProfileId::ARMED || p.id == ProfileId::ALARM));

    if (!frontAllowed)
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
                   (unsigned)n, (unsigned)rawMask);
        return;
    }

    const char *which = (acceptedMask == 0x01)   ? "FRONT"
                        : (acceptedMask == 0x02) ? "BACK"
                        : (acceptedMask == 0x03) ? "FRONT+BACK"
                                                 : "UNKNOWN";

    logSystemf("PIR: ACCEPTED which=%s raw_n=%u raw_mask=0x%02X accepted_mask=0x%02X",
               which, (unsigned)n, (unsigned)rawMask, (unsigned)acceptedMask);

    // ------------------------------------------------------------
    // PIR fram: tyst activity boost i PARKED/ARMED/ALARM.
    // Den ska inte i sig skapa PIR-larm/outbox eller TRIGGERED.
    // Om både front och back kommer samtidigt hanteras backen som skarp.
    // ------------------------------------------------------------
    if ((acceptedMask & 0x01) &&
        (p.id == ProfileId::PARKED || p.id == ProfileId::ARMED || p.id == ProfileId::ALARM))
    {
        activityBoostStartOrExtend(nowMs, "PIR_FRONT");
        acceptedMask &= ~0x01;
    }

    // Om bara PIR fram fanns kvar är vi klara här.
    if (acceptedMask == 0)
        return;

    activityBoostMarkRealEvent();

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

    activityBoostMarkRealEvent();

    if (newProfile == ProfileId::TRAVEL || newProfile == ProfileId::TRIGGERED)
    {
        activityBoostCancel();
    }

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

    uint32_t nowMs = millis();

    // Första kommunikationsförsök en liten stund efter boot
    g_nextCommAtMs = nowMs + 2000UL;

    // Progress startas vid boot.
    g_lastProgressMs = nowMs;
    g_lastSuccessfulMqttConnectMs = 0;
    g_lastSuccessfulPublishMs = 0;

    // Om defaultprofil inte ska hålla anslutning uppe:
    // stäng RF direkt initialt.
    if (!shouldKeepConnectedNow())
    {
        wifiPowerOff();
        modemRfOff();
    }

    victronManagerInit();

    // PIR-ingångar
    pinMode(PIN_PIR_FRONT, INPUT_PULLDOWN);
    pinMode(PIN_PIR_BACK, INPUT_PULLDOWN);

    // Viktigt:
    // Interrupt-läge styrs nu av PIR_RISING_EDGE i config.h.
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_FRONT), isrPirFront, PIR_INTERRUPT_MODE);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_BACK), isrPirBack, PIR_INTERRUPT_MODE);

    stepEnter(Step::STEP_DECIDE, nowMs);
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

    // Avsluta activity boost när tiden gått ut. Event publiceras vid nästa MQTT-cykel.
    activityBoostTick(nowMs);

    // Automatisk TRIGGERED -> ARMED när timeout går ut
    if (currentProfile().id == ProfileId::TRIGGERED &&
        currentProfile().autoReturnMs > 0 &&
        g_triggeredUntilMs > 0 &&
        timeReached(nowMs, g_triggeredUntilMs))
    {
        logSystem("PROFILE: auto return TRIGGERED -> ARMED");
        setProfile(ProfileId::ARMED);
    }

    // Om vi borde vara uppkopplade men inte gjort några riktiga framsteg
    // på länge, trigga recovery.
    if ((shouldKeepConnectedNow() || shouldHoldConnectionForProfilePublish()) &&
        mqttIsConnected() &&
        g_step != Step::STEP_RECOVERY_WAIT)
    {
        if ((uint32_t)(nowMs - g_lastProgressMs) >= PIPE_NO_PROGRESS_LIMIT_MS)
        {
            logSystemf("RECOVERY: no progress for %lu ms while connected",
                       (unsigned long)(nowMs - g_lastProgressMs));
            requestRecovery(RecoveryReason::NO_PROGRESS, nowMs);
            return;
        }
    }

    switch (g_step)
    {
    case Step::STEP_DECIDE:
    {
        bool commDue = timeReached(nowMs, g_nextCommAtMs);
        bool needComm = g_pir.pending || activityBoostHasPendingMqttEvent() || commDue;

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

        // Victron BLE får lägst prioritet: aldrig före PIR/profil/ordinarie kommunikation.
        // Första versionen är tänkt för PARKED med kommunikation avstängd.
        if (victronManagerDue(nowMs, currentProfile()))
        {
            if (currentProfile().victronBleRequiresCommsOff && mqttIsConnected())
            {
                stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
            }
            else
            {
                stepEnter(Step::STEP_VICTRON_BLE_SCAN, nowMs);
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
        else
        {
            stepEnter(Step::STEP_IDLE_WAIT, nowMs);
        }

        break;
    }

    case Step::STEP_NET_ATTACH:
    {
        // ----------------------------------------------------
        // WiFi
        // ----------------------------------------------------
        // WiFi används direkt i WIFI_ONLY samt som primär/backup
        // beroende på valt net_mode. Fallback hanteras av
        // tryFallbackOrRecovery().
        // ----------------------------------------------------
        if (g_netAttemptLink == NetAttemptLink::WIFI)
        {
            bool ok = false;
            if (tickWifiConnect(nowMs, ok))
            {
                if (ok)
                {
                    g_netConnectCountBoot++;
                    g_lastNetConnectMs = nowMs - g_netAttemptStartedMs;

                    markProgress(nowMs, "wifi connect ok");

                    // Vid WiFi kan NTP fungera direkt. Modemklocka hoppar vi över.
                    timeSyncFromNtp(8000);

                    stepEnter(Step::STEP_MQTT_CONNECT, nowMs);
                    break;
                }

                logSystem("PIPELINE: wifi attach failed");
                tryFallbackOrRecovery(RecoveryReason::NET_ATTACH_FAILED, "WIFI_ATTACH_FAILED", nowMs);
                break;
            }

            if (stepTimedOut(nowMs))
            {
                mqttSetNetStatus("NONE", false, false, false, 0, -1, "WIFI_STEP_TIMEOUT");
                logSystem("PIPELINE: WIFI NET_ATTACH step timeout");
                wifiPowerOff();
                tryFallbackOrRecovery(RecoveryReason::NET_ATTACH_FAILED, "WIFI_STEP_TIMEOUT", nowMs);
            }

            break;
        }

        // ----------------------------------------------------
        // SIM/modem – samma logik som tidigare
        // ----------------------------------------------------
        NetResult net;
        bool ok = false;

        // Viktigt:
        // Ticka först. Då hinner vi konsumera DONE_OK / DONE_FAIL
        // innan vi eventuellt startar ett nytt försök.
        if (modemTickConnectData(net, ok))
        {
            if (ok)
            {
                g_netConnectCountBoot++;
                g_lastNetConnectMs = net.connectMs;

                int csq = modemGetSignalQuality();
                mqttUseSimClient();
                mqttSetNetStatus("SIM", false, true, false, 0, csq, "NONE");

                markProgress(nowMs, "net attach ok");

                timeSyncFromModem();
                timeSyncFromNtp(8000);

                stepEnter(Step::STEP_MQTT_CONNECT, nowMs);
                break;
            }

            mqttSetNetStatus("NONE", false, false, false, 0, -1, net.err.c_str());
            logSystem("PIPELINE: net attach failed err=" + net.err);
            String reasonText = net.err.length() > 0 ? net.err : "SIM_ATTACH_FAILED";
            tryFallbackOrRecovery(RecoveryReason::NET_ATTACH_FAILED, reasonText.c_str(), nowMs);
            break;
        }

        // Om inget försök pågår just nu startar vi ett nytt.
        if (!modemIsConnectBusy())
        {
            mqttUseSimClient();
            mqttSetNetStatus("SIM", false, false, false, 0, -1, "SIM_CONNECTING");
            modemStartConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS);
            // markProgress(nowMs, "net attach started");
        }

        // Extra skydd om modemets state machine av någon anledning inte blir klar.
        if (stepTimedOut(nowMs))
        {
            mqttSetNetStatus("NONE", false, false, false, 0, -1, "SIM_STEP_TIMEOUT");
            logSystem("PIPELINE: NET_ATTACH step timeout");
            tryFallbackOrRecovery(RecoveryReason::NET_ATTACH_FAILED, "SIM_STEP_TIMEOUT", nowMs);
        }

        break;
    }

    case Step::STEP_MQTT_CONNECT:
        if (mqttConnect())
        {
            g_lastSuccessfulMqttConnectMs = nowMs;
            g_mqttConnectCountBoot++;

            const char *statusReason = g_netFallbackTried ? g_lastFallbackReason.c_str() : "NONE";
            if (String(mqttGetActiveLink()) == "WIFI")
            {
                mqttSetNetStatus("WIFI", true, false, true, WiFi.RSSI(), -1, statusReason);
            }
            else
            {
                mqttSetNetStatus("SIM", false, true, true, 0, modemGetSignalQuality(), statusReason);
            }

            markProgress(nowMs, "mqtt connect ok");

            // Viktigt:
            // efter connect går vi INTE direkt till publish,
            // utan ger retained desired_profile en chans först.
            stepEnter(Step::STEP_BOOT_PROFILE_SYNC, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            logSystem("PIPELINE: MQTT_CONNECT timeout");
            tryFallbackOrRecovery(RecoveryReason::MQTT_CONNECT_TIMEOUT, "MQTT_CONNECT_TIMEOUT", nowMs);
        }
        break;

    case Step::STEP_BOOT_PROFILE_SYNC:
        // Under detta korta fönstret kör vi mqttLoop så att retained
        // desired_profile faktiskt hinner processas av callbacken.
        if (!mqttLoop())
        {
            requestRecovery(RecoveryReason::MQTT_DROPPED, nowMs);
            break;
        }

        // Om retained profil redan hunnit komma kan vi gå vidare direkt.
        if (mqttHasSeenDesiredProfileThisConnect())
        {
            markProgress(nowMs, "desired profile received");
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
        if (!mqttIsConnected())
        {
            requestRecovery(RecoveryReason::MQTT_DROPPED, nowMs);
            break;
        }

        bool ackOk = mqttPublishPendingProfileAck();
        bool activityOk = activityBoostPublishPending();

        ExtGnssFix fx;
        bool fixOk = buildGpsFromExternal(fx);
        bool gpsOk = mqttPublishGpsSingle(fx, fixOk);
        (void)gpsOk;

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
        bool netStatusOk = mqttPublishNetStatus();
        bool healthOk = mqttPublishHealth(
            g_recoveryCountBoot,
            recoveryReasonName(g_lastRecoveryReason),
            g_netConnectCountBoot,
            g_mqttConnectCountBoot,
            g_lastNetConnectMs,
            mqttHasPendingProfileAck(),
            g_pir.pending);

        // Victron är extra telemetri. Misslyckad Victron-publish ska loggas,
        // men inte dra igång recovery eller störa larmets kärnflöde.
        bool victronOk = mqttPublishVictronStateIfPending();
        if (!victronOk)
        {
            logSystem("MQTT: Victron publish failed/deferred");
        }

        // Treata detta som lyckad publish-cykel om ALIVE gick igenom,
        // nätstatus gick igenom och eventuell profile-ACK också gick igenom.
        bool cycleHealthy = aliveOk && ackOk && activityOk && netStatusOk && healthOk;

        if (cycleHealthy)
        {
            g_lastSuccessfulPublishMs = nowMs;
            markHealthy(nowMs, "publish cycle ok");
        }
        else
        {
            logSystemf("PIPELINE: publish cycle degraded ack_ok=%d activity_ok=%d alive_ok=%d health_ok=%d net_ok=%d pir_ok=%d gps_ok=%d",
                       ackOk ? 1 : 0,
                       activityOk ? 1 : 0,
                       aliveOk ? 1 : 0,
                       healthOk ? 1 : 0,
                       netStatusOk ? 1 : 0,
                       pirOk ? 1 : 0,
                       gpsOk ? 1 : 0);

            // Om central publish inte går igenom vill vi inte ligga kvar
            // i ett halvanslutet läge och hoppas för länge.
            requestRecovery(RecoveryReason::PUBLISH_FAILED, nowMs);
            break;
        }

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

        // Planera nästa kommunikation.
        // Under activity boost körs GPS/MQTT tätare än profilens normalintervall.
        g_nextCommAtMs = nowMs + currentCommIntervalMs(nowMs);

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
        if (!mqttLoop())
        {
            requestRecovery(RecoveryReason::MQTT_DROPPED, nowMs);
            break;
        }

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
        if (!mqttLoop())
        {
            requestRecovery(RecoveryReason::MQTT_DROPPED, nowMs);
            break;
        }

        // Om HA precis har bytt nätläge från t.ex. SIM till WIFI_ONLY
        // ska vi inte vänta på nästa reboot. Koppla ner och gå till DECIDE,
        // så väljer NET_ATTACH rätt länk utifrån det sparade net_mode.
        if (mqttNetModeSwitchRequested())
        {
            logSystem("PIPELINE: net_mode switch requested -> reconnect with selected link");
            mqttClearNetModeSwitchRequested();

            // Koppla ner befintlig MQTT/länk direkt och gå tillbaka till NET_ATTACH.
            // I TRAVEL/TRIGGERED/ALARM vill vi inte hamna i IDLE_WAIT, utan byta länk nu.
            mqttDisconnect();

            if (String(mqttGetActiveLink()) == "WIFI")
            {
                wifiPowerOff();
            }
            else
            {
                modemRfOff();
            }

            stepEnter(Step::STEP_NET_ATTACH, nowMs);
            break;
        }

        if (activityBoostHasPendingMqttEvent())
        {
            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

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
        if (g_pir.pending || activityBoostHasPendingMqttEvent() || timeReached(nowMs, g_nextCommAtMs) || victronManagerDue(nowMs, currentProfile()))
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

    case Step::STEP_VICTRON_BLE_SCAN:
    {
        // Extra skydd: om något kritiskt hann komma in innan scan startar, avbryt BLE.
        if (g_pir.pending || timeReached(nowMs, g_nextCommAtMs))
        {
            logSystem("VICTRON: scan skipped, communication/PIR became due");
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }

        if (currentProfile().victronBleRequiresCommsOff)
        {
            mqttDisconnect();
            wifiPowerOff();
            modemAbortConnectData();
            // Modemet är normalt redan RF_OFF efter föregående kommunikationsfönster.
            // Kör inte CFUN=0 här igen; det kan blockera ~5 s och ge
            // "CFUN=0 failed" precis före BLE-scan.
        }

        victronManagerRunScanOnce(nowMs, currentProfile().victronBleScanSeconds);
        stepEnter(Step::STEP_DECIDE, millis());
        break;
    }

    case Step::STEP_RECOVERY_WAIT:
        performRecovery(nowMs);
        break;

    default:
        requestRecovery(RecoveryReason::STEP_TIMEOUT, nowMs);
        break;
    }
}