#include "modem.h"
#include "config.h"
#include "logging.h"

#include <TinyGsmClient.h>

// ============================================================
// MODEM / UART
// ------------------------------------------------------------
// SerialAT  = UART-kanalen mot SIM7080
// modem     = TinyGSM-wrapper ovanpå UART
// gsmClient = TCP/IP-klient som används av MQTT eller annan IP-trafik
// ============================================================

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
static TinyGsmClient gsmClient(modem);

// ============================================================
// TUNING-KONSTANTER
// ------------------------------------------------------------
// Dessa värden är valda för att minska total uppkopplingstid
// utan att helt ta bort robust fallback-logik.
// ============================================================

// Hur länge vi väntar på SIM i normal startväg.
// Tanken här är: "snabb probe", inte lång blockerande väntan.
static const uint32_t MODEM_SIM_READY_QUICK_WAIT_MS = 1200UL;

// Lite längre SIM-check i fallback-vägen, men fortfarande kort.
static const uint32_t MODEM_SIM_READY_FALLBACK_WAIT_MS = 2000UL;

// Kortare stabiliseringstid efter RF ON i normalläget.
static const uint32_t MODEM_RF_SETTLE_NORMAL_MS = 1500UL;

// Något längre stabilisering i fallback-vägen.
static const uint32_t MODEM_RF_SETTLE_FALLBACK_MS = 2500UL;

// Kort paus efter RF OFF i fallback innan vi konfigurerar / startar om RF.
static const uint32_t MODEM_RF_RESTART_PAUSE_MS = 1200UL;

// ============================================================
// INTERNA HJÄLPFUNKTIONER
// ============================================================

// ------------------------------------------------------------
// Skicka en PWRKEY-puls till modemet.
// Den här hårdvaran använder LOW -> HIGH -> LOW.
// pulseHighMs anger hur länge pinnen hålls HIGH.
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
// mode:
//   0 = RF av
//   1 = full funktion / RF på
//
// Returnerar true om AT-kommandot gav OK.
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
// Vänta tills modemet svarar på AT.
// Om det dröjer länge försöker vi trigga modemet med PWRKEY.
// ------------------------------------------------------------
static bool modemWaitForAT(uint32_t timeoutMs)
{
    uint32_t start = millis();
    int retry = 0;

    logSystem("MODEM: waiting for AT...");

    while (millis() - start < timeoutMs)
    {
        if (modem.testAT(1000))
        {
            logSystem("MODEM: AT OK");
            return true;
        }

        retry++;
        logSystem("MODEM: no AT yet, retry=" + String(retry));
        delay(1000);

        // Var 6:e försök: skicka en kort PWRKEY-puls
        if ((retry % 6) == 0)
        {
            logSystem("MODEM: PWRKEY pulse to wake/start modem");
            modemPwrKeyPulse(1000);
        }
    }

    logSystem("MODEM: AT FAILED after " + String(timeoutMs) + " ms");
    return false;
}

// ------------------------------------------------------------
// Vänta på nätregistrering.
// Returnerar true när modemet rapporterar att nätet är anslutet.
// ------------------------------------------------------------
static bool modemWaitForNetwork(uint32_t timeoutMs)
{
    uint32_t start = millis();
    uint32_t lastLogMs = 0;

    logSystem("MODEM: wait for network registration...");

    while (millis() - start < timeoutMs)
    {
        if (modem.isNetworkConnected())
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: network registered (CSQ=" + String(csq) + ")");
            return true;
        }

        uint32_t elapsedMs = millis() - start;

        // Logga ungefär var 10:e sekund
        if (elapsedMs - lastLogMs >= 10000UL)
        {
            lastLogMs = elapsedMs;
            int csq = modem.getSignalQuality();
            logSystem("MODEM: still waiting net reg... t=" + String(elapsedMs / 1000) + "s CSQ=" + String(csq));
        }

        delay(1000);
    }

    logSystem("MODEM: network registration TIMEOUT");
    return false;
}

// ------------------------------------------------------------
// Sätt modemets nätläge och APN.
// Detta är grundkonfiguration före dataanslutning.
//
// Notera:
// Vi lägger gärna denna konfiguration tidigt i flödet, så att
// modemet redan har rätt APN/PDP-inställning när nätregistrering
// och databärare sedan aktiveras.
// ------------------------------------------------------------
static void modemConfigureRadioAndApn(const char *apn)
{
    // Auto network mode
    modem.setNetworkMode(2);

    // Föredra CAT-M + NB-IoT
    modem.setPreferredMode(3);

    // PDP context: context 1, IP, valt APN
    modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
    if (modem.waitResponse(5000UL) != 1)
    {
        logSystem("MODEM: CGDCONT failed");
    }

    // SIM7080-specifik data bearer config
    modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
    if (modem.waitResponse(5000UL) != 1)
    {
        logSystem("MODEM: CNCFG failed");
    }
}

// ------------------------------------------------------------
// Slå på RF och ge modemet lite tid att stabilisera sig innan
// vi försöker nätregistrera.
//
// settleMs hålls nu kortare i normalläget för att minska total
// uppkopplingstid. Fallback-vägen kan använda lite längre tid.
// ------------------------------------------------------------
static void modemEnableRfWithSettle(uint32_t cfunTimeoutMs, uint32_t settleMs)
{
    if (!modemSetCfun(1, cfunTimeoutMs))
    {
        logSystem("MODEM: CFUN=1 did not confirm, continuing anyway");
    }

    logSystem("MODEM: RF ON, settling " + String(settleMs) + " ms");
    delay(settleMs);
}

// ------------------------------------------------------------
// Aktivera databärare.
// Returnerar true om data redan var aktiv eller blev aktiv.
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

// ============================================================
// PUBLIKA FUNKTIONER
// ============================================================

// ------------------------------------------------------------
// Initiera UART och modemrelaterade GPIO.
// DTR hålls LOW för att hålla modemet vaket.
// ------------------------------------------------------------
void modemInitUartAndPins()
{
    logSystem("MODEM: init UART & pins");

    SerialAT.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_RI_PIN, INPUT);

    // LOW = håll modemet vaket i denna kodbas
    digitalWrite(BOARD_MODEM_DTR_PIN, LOW);

    // Säker grundnivå för PWRKEY
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
}

// ------------------------------------------------------------
// Anslut modemet till mobilnät och aktivera data.
// out fylls med IP, signalstyrka och ev. felkod.
//
// Flöde:
// 1. Säkerställ AT-kontakt
// 2. Gör en kort SIM-probe (ej lång blockerande väntan)
// 3. Om nät redan finns: återanvänd det
// 4. Annars normal attach
// 5. Om normal attach misslyckas: fallback med RF restart
// 6. Aktivera databärare
// 7. Läs ut IP och CSQ
//
// Viktig ändring:
// SIM-ready är inte längre ett långt blockerande steg i normalläget.
// ------------------------------------------------------------
bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out)
{
    out.ip = "";
    out.csq = -1;
    out.err = "";

    uint32_t tStart = millis();

    // 1) Säkerställ kontakt med modemet
    if (!modemWaitForAT(30000UL))
    {
        out.err = "no_at";
        return false;
    }

    // 3) Återanvänd nätregistrering om vi redan har den
    if (!modem.isNetworkConnected())
    {
        logSystem("MODEM: not network connected -> try normal attach");

        // Grundkonfiguration tidigt i flödet.
        modemConfigureRadioAndApn(apn);

        // RF på + kortare stabilisering än tidigare
        modemEnableRfWithSettle(20000UL, MODEM_RF_SETTLE_NORMAL_MS);

        // Första försök: begränsa timeout så fallback kommer snabbare
        uint32_t firstTryTimeoutMs = netRegTimeoutMs;
        if (firstTryTimeoutMs > 45000UL)
        {
            firstTryTimeoutMs = 45000UL;
        }

        if (!modemWaitForNetwork(firstTryTimeoutMs))
        {
            logSystem("MODEM: normal attach failed -> fallback with RF restart");

            // Fallback:
            // 1. RF av
            // 2. kort paus
            // 3. konfigurera igen
            // 4. RF på + lite längre settle
            modemSetCfun(0, 20000UL);
            delay(MODEM_RF_RESTART_PAUSE_MS);

            modemConfigureRadioAndApn(apn);
            modemEnableRfWithSettle(20000UL, MODEM_RF_SETTLE_FALLBACK_MS);

            if (!modemWaitForNetwork(netRegTimeoutMs))
            {
                out.err = "net_timeout";
                return false;
            }
        }
    }
    else
    {
        logSystem("MODEM: already network connected, reusing registration");
    }

    // 4) Aktivera databärare
    if (!modemActivateData(dataAttachTimeoutMs))
    {
        out.err = "data_attach_failed";
        return false;
    }

    // 5) Läs ut status
    bool dataConnected = modem.isGprsConnected();
    logSystem(String("MODEM: data status: ") + (dataConnected ? "connected" : "NOT connected"));

    IPAddress ip = modem.localIP();
    out.ip = ip.toString();

    out.csq = modem.getSignalQuality();

    logSystem("MODEM: Local IP: " + out.ip);
    logSystem("MODEM: CSQ: " + String(out.csq));

    uint32_t tTotal = millis() - tStart;
    logSystem("NET_CONNECT: SUCCESS, T_net=" + String(tTotal) +
              " ms, IP=" + out.ip + ", CSQ=" + String(out.csq));

    return true;
}

// ------------------------------------------------------------
// Returnerar TinyGSM-klienten som används för TCP/MQTT.
// ------------------------------------------------------------
Client &modemGetClient()
{
    return gsmClient;
}

// ------------------------------------------------------------
// Läs modemets klocka via AT+CCLK?
// Förväntat resultat exempelvis:
//   +CCLK: "25/12/13,19:22:50+04"
//
// Returnerar själva innehållet mellan citattecknen i outCclk.
// ------------------------------------------------------------
bool modemGetCclk(String &outCclk, uint32_t timeoutMs)
{
    outCclk = "";

    // Töm gammal UART-data innan vi skickar kommandot
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
                        // Tvinga avslut av yttre loop
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

// ------------------------------------------------------------
// Stäng av radiofunktionen mellan kommunikationsfönster.
// Modemet är fortfarande strömsatt, men RF är av.
// ------------------------------------------------------------
bool modemRfOff()
{
    logSystem("MODEM: RF OFF (CFUN=0)");
    return modemSetCfun(0, 5000UL);
}

// ------------------------------------------------------------
// Slå på radiofunktionen igen.
// Här används bara ett enkelt CFUN=1.
// Mer robust attach-logik ligger i modemConnectData().
// ------------------------------------------------------------
bool modemRfOn()
{
    logSystem("MODEM: RF ON (CFUN=1)");
    return modemSetCfun(1, 5000UL);
}

// ------------------------------------------------------------
// Gör en full power-cycle av modemet via PWRKEY.
// offMs  = väntetid mellan av/på
// bootMs = tid att låta modemet starta innan nästa steg
// ------------------------------------------------------------
void modemPowerCycle(uint32_t offMs, uint32_t bootMs)
{
    logSystem("MODEM: power cycle start");

    // Försök först stänga av RF snyggt, men det är inte kritiskt
    modemSetCfun(0, 5000UL);

    // Lång puls för att toggla power
    logSystem("MODEM: PWRKEY long pulse (power toggle OFF)");
    modemPwrKeyPulse(1500);

    delay(offMs);

    // Ny puls för att starta igen
    logSystem("MODEM: PWRKEY pulse (power ON)");
    modemPwrKeyPulse(1200);

    // Vänta på att modemet ska boota
    delay(bootMs);

    logSystem("MODEM: power cycle done");
}