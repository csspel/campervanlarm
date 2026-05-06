#include "modem.h"
#include "config.h"
#include "logging.h"

#include <TinyGsmClient.h>

// ============================================================
// MODEM / UART
// ============================================================

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
static TinyGsmClient gsmClient(modem);

// ============================================================
// TUNING-KONSTANTER
// ============================================================

static const uint32_t MODEM_RF_SETTLE_NORMAL_MS = 1500UL;
static const uint32_t MODEM_RF_SETTLE_FALLBACK_MS = 2500UL;
static const uint32_t MODEM_RF_RESTART_PAUSE_MS = 1200UL;

// ============================================================
// INTERN CONNECT-STATE
// ============================================================

struct ModemConnectContext
{
    bool busy = false;
    ModemConnectState state = ModemConnectState::IDLE;

    String apn;
    uint32_t netRegTimeoutMs = 0;
    uint32_t dataAttachTimeoutMs = 0;

    uint32_t startedAtMs = 0;
    uint32_t stateStartedAtMs = 0;
    uint32_t stateDeadlineMs = 0;

    bool fallbackUsed = false;
    bool success = false;

    NetResult result;
};

static ModemConnectContext g_conn;

// ============================================================
// INTERNA HJÄLPFUNKTIONER
// ============================================================

static inline bool timeReached(uint32_t nowMs, uint32_t targetMs)
{
    return (int32_t)(nowMs - targetMs) >= 0;
}

static const char *connectStateName(ModemConnectState s)
{
    switch (s)
    {
    case ModemConnectState::IDLE:
        return "IDLE";
    case ModemConnectState::WAIT_AT:
        return "WAIT_AT";
    case ModemConnectState::CONFIGURE_RADIO:
        return "CONFIGURE_RADIO";
    case ModemConnectState::RF_SETTLE:
        return "RF_SETTLE";
    case ModemConnectState::WAIT_NET_FIRST:
        return "WAIT_NET_FIRST";
    case ModemConnectState::RF_RESTART_OFF:
        return "RF_RESTART_OFF";
    case ModemConnectState::RF_RESTART_PAUSE:
        return "RF_RESTART_PAUSE";
    case ModemConnectState::RF_RESTART_RECONFIGURE:
        return "RF_RESTART_RECONFIGURE";
    case ModemConnectState::RF_RESTART_SETTLE:
        return "RF_RESTART_SETTLE";
    case ModemConnectState::WAIT_NET_FALLBACK:
        return "WAIT_NET_FALLBACK";
    case ModemConnectState::ACTIVATE_DATA:
        return "ACTIVATE_DATA";
    case ModemConnectState::READ_STATUS:
        return "READ_STATUS";
    case ModemConnectState::DONE_OK:
        return "DONE_OK";
    case ModemConnectState::DONE_FAIL:
        return "DONE_FAIL";
    default:
        return "UNKNOWN";
    }
}

static void connectEnterState(ModemConnectState newState, uint32_t nowMs, uint32_t timeoutMs)
{
    ModemConnectState old = g_conn.state;
    g_conn.state = newState;
    g_conn.stateStartedAtMs = nowMs;
    g_conn.stateDeadlineMs = (timeoutMs > 0) ? (nowMs + timeoutMs) : 0;

    if (timeoutMs > 0)
    {
        logSystemf("MODEM: connect state %s -> %s timeout=%lu ms",
                   connectStateName(old),
                   connectStateName(newState),
                   (unsigned long)timeoutMs);
    }
    else
    {
        logSystemf("MODEM: connect state %s -> %s",
                   connectStateName(old),
                   connectStateName(newState));
    }
}

static bool connectStateTimedOut(uint32_t nowMs)
{
    if (g_conn.stateDeadlineMs == 0)
        return false;

    return timeReached(nowMs, g_conn.stateDeadlineMs);
}

// ------------------------------------------------------------
// Skicka en PWRKEY-puls till modemet.
// ------------------------------------------------------------
static void modemPwrKeyPulse(uint32_t pulseHighMs)
{
    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);

    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(100);

    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(pulseHighMs);

    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
}

// ------------------------------------------------------------
// Sätt CFUN-läge.
// ------------------------------------------------------------
static bool modemSetCfun(uint8_t mode, uint32_t timeoutMs)
{
    modem.sendAT("+CFUN=", mode);
    int r = modem.waitResponse(timeoutMs);

    if (r == 1)
    {
        return true;
    }

    logSystem("MODEM: CFUN=" + String(mode) + " failed (waitResponse=" + String(r) + ")");
    return false;
}

// ------------------------------------------------------------
// Ett enda snabbt AT-test.
// I stället för gammal blockande loop gör vi ett litet steg per tick.
// ------------------------------------------------------------
static bool modemTestATOnce()
{
    return modem.testAT(500);
}

// ------------------------------------------------------------
// Sätt modemets nätläge och APN.
// ------------------------------------------------------------
static void modemConfigureRadioAndApn(const char *apn)
{
    modem.setNetworkMode(2);
    modem.setPreferredMode(3);

    modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
    if (modem.waitResponse(5000UL) != 1)
    {
        logSystem("MODEM: CGDCONT failed");
    }

    modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
    if (modem.waitResponse(5000UL) != 1)
    {
        logSystem("MODEM: CNCFG failed");
    }
}

// ------------------------------------------------------------
// Aktivera databärare.
// Fortfarande blockande på AT-svaret, men nu som ett avgränsat steg.
// ------------------------------------------------------------
static bool modemActivateData(uint32_t timeoutMs)
{
    bool connectedBefore = modem.isGprsConnected();
    logSystem(String("MODEM: data status before CNACT: ") + (connectedBefore ? "connected" : "NOT connected"));

    if (connectedBefore)
    {
        logSystem("MODEM: data already connected, skip CNACT");
        return true;
    }

    logSystem("MODEM: activate data bearer (+CNACT=0,1)");
    modem.sendAT("+CNACT=0,1");

    if (modem.waitResponse(timeoutMs) != 1)
    {
        logSystem("MODEM: CNACT failed, re-checking data state");

        bool connectedAfterFail = modem.isGprsConnected();
        logSystem(String("MODEM: data status after CNACT fail: ") + (connectedAfterFail ? "connected" : "NOT connected"));

        if (connectedAfterFail)
        {
            logSystem("MODEM: treating CNACT fail as non-fatal (data is connected)");
            return true;
        }

        logSystem("MODEM: data attach FAILED");
        return false;
    }

    bool connectedAfterOk = modem.isGprsConnected();
    logSystem(String("MODEM: data status after CNACT OK: ") + (connectedAfterOk ? "connected" : "NOT connected"));

    return connectedAfterOk;
}

static void connectFinishSuccess(uint32_t nowMs)
{
    uint32_t totalMs = nowMs - g_conn.startedAtMs;

    g_conn.success = true;
    g_conn.busy = false;
    g_conn.result.connectMs = totalMs;
    connectEnterState(ModemConnectState::DONE_OK, nowMs, 0);

    logSystem("NET_CONNECT: SUCCESS, T_net=" + String(totalMs) +
              " ms, IP=" + g_conn.result.ip + ", CSQ=" + String(g_conn.result.csq));
}

static void connectFinishFail(const String &err, uint32_t nowMs)
{
    uint32_t totalMs = nowMs - g_conn.startedAtMs;

    g_conn.result.err = err;
    g_conn.result.connectMs = totalMs;
    g_conn.success = false;
    g_conn.busy = false;
    connectEnterState(ModemConnectState::DONE_FAIL, nowMs, 0);

    logSystem("NET_CONNECT: FAIL, err=" + err + ", T_net=" + String(totalMs) + " ms");
}

// ============================================================
// PUBLIKA FUNKTIONER
// ============================================================

void modemInitUartAndPins()
{
    logSystem("MODEM: init UART & pins");

    SerialAT.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_RI_PIN, INPUT);

    digitalWrite(BOARD_MODEM_DTR_PIN, LOW);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
}

void modemStartConnectData(const char *apn,
                           uint32_t netRegTimeoutMs,
                           uint32_t dataAttachTimeoutMs)
{
    uint32_t nowMs = millis();

    g_conn.result.connectMs = 0;
    g_conn = ModemConnectContext{};
    g_conn.busy = true;
    g_conn.state = ModemConnectState::IDLE;
    g_conn.apn = apn ? apn : "";
    g_conn.netRegTimeoutMs = netRegTimeoutMs;
    g_conn.dataAttachTimeoutMs = dataAttachTimeoutMs;
    g_conn.startedAtMs = nowMs;
    g_conn.stateStartedAtMs = nowMs;
    g_conn.result.ip = "";
    g_conn.result.csq = -1;
    g_conn.result.err = "";

    logSystem("MODEM: start non-blocking connect");
    connectEnterState(ModemConnectState::WAIT_AT, nowMs, 30000UL);
}

bool modemTickConnectData(NetResult &out, bool &success)
{
    uint32_t nowMs = millis();

    out = NetResult{};
    success = false;

    // Om vi redan står i färdigt läge från tidigare anrop:
    if (!g_conn.busy)
    {
        if (g_conn.state == ModemConnectState::DONE_OK)
        {
            out = g_conn.result;
            success = true;
            g_conn.state = ModemConnectState::IDLE;
            return true;
        }

        if (g_conn.state == ModemConnectState::DONE_FAIL)
        {
            out = g_conn.result;
            success = false;
            g_conn.state = ModemConnectState::IDLE;
            return true;
        }

        return false;
    }

    switch (g_conn.state)
    {
    case ModemConnectState::WAIT_AT:
        if (modemTestATOnce())
        {
            logSystem("MODEM: AT OK");
            connectEnterState(ModemConnectState::CONFIGURE_RADIO, nowMs, 0);
            break;
        }

        if (connectStateTimedOut(nowMs))
        {
            connectFinishFail("no_at", nowMs);
            out = g_conn.result;
            success = false;
            g_conn.state = ModemConnectState::IDLE;
            return true;
        }
        break;

    case ModemConnectState::CONFIGURE_RADIO:
        if (modem.isNetworkConnected())
        {
            logSystem("MODEM: already network connected, reusing registration");
            connectEnterState(ModemConnectState::ACTIVATE_DATA, nowMs, 0);
            break;
        }

        logSystem("MODEM: not network connected -> configure radio/APN");
        modemConfigureRadioAndApn(g_conn.apn.c_str());

        if (!modemSetCfun(1, 20000UL))
        {
            logSystem("MODEM: CFUN=1 did not confirm, continuing anyway");
        }

        logSystem("MODEM: RF ON, settling " + String(MODEM_RF_SETTLE_NORMAL_MS) + " ms");
        connectEnterState(ModemConnectState::RF_SETTLE, nowMs, MODEM_RF_SETTLE_NORMAL_MS);
        break;

    case ModemConnectState::RF_SETTLE:
        if (connectStateTimedOut(nowMs))
        {
            uint32_t firstTryTimeoutMs = g_conn.netRegTimeoutMs;
            if (firstTryTimeoutMs > 45000UL)
            {
                firstTryTimeoutMs = 45000UL;
            }

            logSystem("MODEM: wait for network registration (first try)");
            connectEnterState(ModemConnectState::WAIT_NET_FIRST, nowMs, firstTryTimeoutMs);
        }
        break;

    case ModemConnectState::WAIT_NET_FIRST:
        if (modem.isNetworkConnected())
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: network registered (CSQ=" + String(csq) + ")");
            connectEnterState(ModemConnectState::ACTIVATE_DATA, nowMs, 0);
            break;
        }

        if (((nowMs - g_conn.stateStartedAtMs) % 10000UL) < 200UL)
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: still waiting net reg... t=" + String((nowMs - g_conn.stateStartedAtMs) / 1000) +
                      "s CSQ=" + String(csq));
        }

        if (connectStateTimedOut(nowMs))
        {
            logSystem("MODEM: normal attach failed -> fallback with RF restart");
            connectEnterState(ModemConnectState::RF_RESTART_OFF, nowMs, 0);
        }
        break;

    case ModemConnectState::RF_RESTART_OFF:
        modemSetCfun(0, 20000UL);
        connectEnterState(ModemConnectState::RF_RESTART_PAUSE, nowMs, MODEM_RF_RESTART_PAUSE_MS);
        break;

    case ModemConnectState::RF_RESTART_PAUSE:
        if (connectStateTimedOut(nowMs))
        {
            connectEnterState(ModemConnectState::RF_RESTART_RECONFIGURE, nowMs, 0);
        }
        break;

    case ModemConnectState::RF_RESTART_RECONFIGURE:
        modemConfigureRadioAndApn(g_conn.apn.c_str());

        if (!modemSetCfun(1, 20000UL))
        {
            logSystem("MODEM: CFUN=1 fallback did not confirm, continuing anyway");
        }

        g_conn.fallbackUsed = true;
        logSystem("MODEM: RF ON fallback, settling " + String(MODEM_RF_SETTLE_FALLBACK_MS) + " ms");
        connectEnterState(ModemConnectState::RF_RESTART_SETTLE, nowMs, MODEM_RF_SETTLE_FALLBACK_MS);
        break;

    case ModemConnectState::RF_RESTART_SETTLE:
        if (connectStateTimedOut(nowMs))
        {
            logSystem("MODEM: wait for network registration (fallback)");
            connectEnterState(ModemConnectState::WAIT_NET_FALLBACK, nowMs, g_conn.netRegTimeoutMs);
        }
        break;

    case ModemConnectState::WAIT_NET_FALLBACK:
        if (modem.isNetworkConnected())
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: network registered after fallback (CSQ=" + String(csq) + ")");
            connectEnterState(ModemConnectState::ACTIVATE_DATA, nowMs, 0);
            break;
        }

        if (((nowMs - g_conn.stateStartedAtMs) % 10000UL) < 200UL)
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: fallback waiting net reg... t=" + String((nowMs - g_conn.stateStartedAtMs) / 1000) +
                      "s CSQ=" + String(csq));
        }

        if (connectStateTimedOut(nowMs))
        {
            connectFinishFail("net_timeout", nowMs);
            out = g_conn.result;
            success = false;
            g_conn.state = ModemConnectState::IDLE;
            return true;
        }
        break;

    case ModemConnectState::ACTIVATE_DATA:
        if (!modemActivateData(g_conn.dataAttachTimeoutMs))
        {
            connectFinishFail("data_attach_failed", nowMs);
            out = g_conn.result;
            success = false;
            g_conn.state = ModemConnectState::IDLE;
            return true;
        }

        connectEnterState(ModemConnectState::READ_STATUS, nowMs, 0);
        break;

    case ModemConnectState::READ_STATUS:
    {
        bool dataConnected = modem.isGprsConnected();
        logSystem(String("MODEM: data status: ") + (dataConnected ? "connected" : "NOT connected"));

        IPAddress ip = modem.localIP();
        g_conn.result.ip = ip.toString();
        g_conn.result.csq = modem.getSignalQuality();
        g_conn.result.err = "";

        logSystem("MODEM: Local IP: " + g_conn.result.ip);
        logSystem("MODEM: CSQ: " + String(g_conn.result.csq));

        connectFinishSuccess(nowMs);

        out = g_conn.result;
        success = true;
        g_conn.state = ModemConnectState::IDLE;
        return true;
    }

    case ModemConnectState::DONE_OK:
        out = g_conn.result;
        success = true;
        g_conn.state = ModemConnectState::IDLE;
        return true;

    case ModemConnectState::DONE_FAIL:
        out = g_conn.result;
        success = false;
        g_conn.state = ModemConnectState::IDLE;
        return true;

    case ModemConnectState::IDLE:
    default:
        break;
    }

    return false;
}

void modemAbortConnectData()
{
    if (g_conn.busy)
    {
        logSystem("MODEM: abort connect");
    }

    g_conn = ModemConnectContext{};
}

bool modemIsConnectBusy()
{
    return g_conn.busy;
}

ModemConnectState modemGetConnectState()
{
    return g_conn.state;
}

// ------------------------------------------------------------
// Gammal blockerande funktion.
// Behålls tills pipeline bytts över.
// ------------------------------------------------------------
bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out)
{
    bool success = false;

    modemStartConnectData(apn, netRegTimeoutMs, dataAttachTimeoutMs);

    while (modemIsConnectBusy())
    {
        if (modemTickConnectData(out, success))
        {
            return success;
        }
        delay(50);
    }

    // Om state redan var DONE_* när while inte kördes.
    if (modemTickConnectData(out, success))
    {
        return success;
    }

    out.err = "unexpected_abort";
    return false;
}

Client &modemGetClient()
{
    return gsmClient;
}

int modemGetSignalQuality()
{
    int csq = modem.getSignalQuality();
    if (csq < 0)
    {
        return -1;
    }

    return csq;
}

bool modemGetCclk(String &outCclk, uint32_t timeoutMs)
{
    outCclk = "";

    while (SerialAT.available())
    {
        SerialAT.read();
    }

    SerialAT.println("AT+CCLK?");

    uint32_t start = millis();
    String line;
    String payload;

    while (millis() - start < timeoutMs)
    {
        while (SerialAT.available())
        {
            char c = (char)SerialAT.read();

            if (c == '\r')
            {
                continue;
            }

            if (c == '\n')
            {
                line.trim();

                if (line.length() > 0)
                {
                    if (line.startsWith("+CCLK:"))
                    {
                        payload = line;
                    }
                    else if (line == "OK")
                    {
                        start = 0;
                        timeoutMs = 0;
                        break;
                    }
                }

                line = "";
            }
            else
            {
                line += c;
            }
        }

        delay(10);
    }

    if (!payload.startsWith("+CCLK:"))
    {
        return false;
    }

    int q1 = payload.indexOf('"');
    int q2 = payload.lastIndexOf('"');

    if (q1 < 0 || q2 <= q1)
    {
        return false;
    }

    outCclk = payload.substring(q1 + 1, q2);
    outCclk.trim();

    return outCclk.length() >= 17;
}

bool modemRfOff()
{
    logSystem("MODEM: RF OFF (CFUN=0)");
    return modemSetCfun(0, 5000UL);
}

bool modemRfOn()
{
    logSystem("MODEM: RF ON (CFUN=1)");
    return modemSetCfun(1, 5000UL);
}

void modemPowerCycle(uint32_t offMs, uint32_t bootMs)
{
    logSystem("MODEM: power cycle start");

    modemSetCfun(0, 5000UL);

    logSystem("MODEM: PWRKEY long pulse (power toggle OFF)");
    modemPwrKeyPulse(1500);

    delay(offMs);

    logSystem("MODEM: PWRKEY pulse (power ON)");
    modemPwrKeyPulse(1200);

    delay(bootMs);

    logSystem("MODEM: power cycle done");
}