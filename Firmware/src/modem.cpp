#include "modem.h"
#include "config.h"
#include "logging.h"

#include <TinyGsmClient.h>

// UART mot modem
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
static TinyGsmClient gsmClient(modem);

// ====== interna hjälpfunktioner ======

static bool modemWaitForAT(uint32_t timeoutMs)
{
    uint32_t start = millis();
    uint32_t lastLog = 0;
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

        // Om vi försökt några gånger: PWRKEY-puls
        if (retry % 6 == 0)
        {
            logSystem("MODEM: PWRKEY pulse to start modem");
            pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
            delay(1000);
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
        }
    }

    logSystem("MODEM: AT FAILED after " + String(timeoutMs) + " ms");
    return false;
}

static bool modemWaitForSimReady(uint32_t timeoutMs)
{
    uint32_t start = millis();

    while (millis() - start < timeoutMs)
    {
        auto st = modem.getSimStatus();
        if (st == SIM_READY)
        {
            logSystem("MODEM: SIM ready");
            return true;
        }
        logSystem("MODEM: SIM not ready yet (status=" + String((int)st) + ")");
        delay(1000);
    }

    logSystem("MODEM: SIM still NOT ready after " + String(timeoutMs) + " ms");
    return false;
}

static bool modemWaitForNetwork(uint32_t timeoutMs)
{
    uint32_t start = millis();
    uint32_t lastLog = 0;

    logSystem("MODEM: wait for network registration...");

    while (millis() - start < timeoutMs)
    {
        if (modem.isNetworkConnected())
        {
            int csq = modem.getSignalQuality();
            logSystem("MODEM: network registered (CSQ=" + String(csq) + ")");
            return true;
        }
        // En liten "progress" utan att spamma loggar
        Serial.print(".");
        delay(1000);
        // Var ~10:e sekund: skriv mer info
        uint32_t elapsed = millis() - start;
        if (elapsed - lastLog >= 10000UL)
        {
            lastLog = elapsed;
            int csq = modem.getSignalQuality();
            logSystem("MODEM: still waiting net reg... t=" + String(elapsed / 1000) + "s CSQ=" + String(csq));
        }
    }

    logSystem("MODEM: network registration TIMEOUT");
    return false;
}

static bool modemActivateData(uint32_t timeoutMs)
{
    // Kolla först om vi redan har datalänk
    bool gprsBefore = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status before CNACT: ") + (gprsBefore ? "connected" : "NOT connected"));

    if (gprsBefore)
    {
        logSystem("MODEM: GPRS already connected, skip CNACT");
        return true;
    }

    // Försök aktivera data
    logSystem("MODEM: activate data bearer (+CNACT=0,1)");
    modem.sendAT("+CNACT=0,1");
    if (modem.waitResponse(timeoutMs) != 1)
    {
        logSystem("MODEM: CNACT failed, re-checking GPRS state");

        bool gprsAfter = modem.isGprsConnected();
        logSystem(String("MODEM: GPRS status after CNACT fail: ") + (gprsAfter ? "connected" : "NOT connected"));

        if (gprsAfter)
        {
            logSystem("MODEM: treating CNACT fail as non-fatal (GPRS is connected)");
            return true;
        }

        logSystem("MODEM: data attach really FAILED");
        return false;
    }

    bool gprsAfter = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status after CNACT OK: ") + (gprsAfter ? "connected" : "NOT connected"));
    return gprsAfter;
}

// --- CFUN helper (rätt sätt i din kodbas) ---
static bool modemSetCfun(uint8_t mode, uint32_t timeoutMs)
{
    // mode: 0 = RF off, 1 = full functionality
    modem.sendAT("+CFUN=", mode);
    int r = modem.waitResponse(timeoutMs);
    if (r == 1)
        return true;

    // logga men låt caller avgöra om det är fatal
    logSystem(String("MODEM: CFUN=") + String(mode) + " failed (waitResponse=" + String(r) + ")");
    return false;
}

// ====== publika funktioner ======

void modemInitUartAndPins()
{
    logSystem("MODEM: init UART & pins");

    SerialAT.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_RI_PIN, INPUT);

    // Håll modem vaken
    digitalWrite(BOARD_MODEM_DTR_PIN, LOW);
}

bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out)
{
    out.ip = "";
    out.csq = -1;
    out.err = "";

    uint32_t tStart = millis();

    // 1) Säkerställ AT-kontakt
    if (!modemWaitForAT(30000UL))
    {
        out.err = "no_at";
        return false;
    }

    // 2) Mjuk SIM-check
    // if (!modemWaitForSimReady(20000UL))
    //{
    //    logSystem("MODEM: SIM check failed, fortsätter ändå (litar på nätuppkopplingstest)");
    //}

    // 3) Kolla nätregistrering
    bool alreadyNet = modem.isNetworkConnected();
    if (!alreadyNet)
    {
        // Vi har ofta redan slagit CFUN=1 i pipeline (STEP_RF_ON). Försök därför
        // en "snabb väg" först utan att toggla RF (CFUN=0/1) i onödan.
        logSystem("MODEM: not network connected → try attach without RF toggle");

        modem.setNetworkMode(2);   // auto
        modem.setPreferredMode(3); // CAT-M + NB-IoT

        // Säkerställ att RF är på (idempotent)
        logSystem("MODEM: ensure RF ON (CFUN=1)");
        modemSetCfun(1, 20000UL);

        // APN (idempotent)
        logSystem("MODEM: set APN via CGDCONT/CNCFG");
        // CGDCONT: PDP type "IP" + APN
        modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
        if (modem.waitResponse(5000UL) != 1)
        {
            logSystem("MODEM: CGDCONT failed");
        }

        modem.sendAT("+CNCFG=0,1,"
                     ", apn, "
                     "");
        if (modem.waitResponse(5000UL) != 1)
        {
            logSystem("MODEM: CNCFG failed");
        }

        // Vänta registrering
        if (!modemWaitForNetwork(netRegTimeoutMs))
        {
            // Fallback: gör full setup med RF OFF/ON
            logSystem("MODEM: attach failed → doing full CFUN/APN setup");

            logSystem("MODEM: disable RF (CFUN=0)");
            modemSetCfun(0, 20000UL); // ignorerar fel (loggas i helpern)

            modem.setNetworkMode(2);   // auto
            modem.setPreferredMode(3); // CAT-M + NB-IoT

            logSystem("MODEM: set APN via CGDCONT/CNCFG");
            // CGDCONT: PDP type "IP" + APN
            modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
            if (modem.waitResponse(5000UL) != 1)
            {
                logSystem("MODEM: CGDCONT failed");
            }

            modem.sendAT("+CNCFG=0,1,"
                         ", apn, "
                         "");
            if (modem.waitResponse(5000UL) != 1)
            {
                logSystem("MODEM: CNCFG failed");
            }

            logSystem("MODEM: enable RF (CFUN=1)");
            modemSetCfun(1, 20000UL);
            delay(1000);

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

    // 4) Data
    if (!modemActivateData(dataAttachTimeoutMs))
    {
        out.err = "data_attach_failed";
        return false;
    }

    // 5) IP, CSQ
    bool gprs = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status: ") + (gprs ? "connected" : "NOT connected"));

    IPAddress ip = modem.localIP();
    out.ip = ip.toString();

    int csq = modem.getSignalQuality();
    out.csq = csq;

    logSystem("MODEM: Local IP: " + out.ip);
    logSystem("MODEM: CSQ: " + String(csq));

    uint32_t tTotal = millis() - tStart;
    logSystem("NET_CONNECT: SUCCESS, T_net=" + String(tTotal) +
              " ms, IP=" + out.ip + ", CSQ=" + String(csq));

    return true;
}

Client &modemGetClient()
{
    return gsmClient;
}

// ---- Clock via AT+CCLK? -------------------------------------------------
bool modemGetCclk(String &outCclk, uint32_t timeoutMs)
{
    while (SerialAT.available())
        SerialAT.read();

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
                continue;
            if (c == '\n')
            {
                line.trim();
                if (line.length() > 0)
                {
                    if (line.startsWith("+CCLK:"))
                    {
                        payload = line;
                    }
                    if (line == "OK")
                    {
                        start = timeoutMs + start;
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
        return false;

    int q1 = payload.indexOf('"');
    int q2 = payload.lastIndexOf('"');
    if (q1 < 0 || q2 <= q1)
        return false;

    outCclk = payload.substring(q1 + 1, q2);
    outCclk.trim();
    return outCclk.length() >= 17;
}

// ---- RF control between comm windows (no deep sleep) ---------------------
bool modemRfOff()
{
    logSystem("MODEM: RF OFF (CFUN=0)");
    // Om CFUN=0 misslyckas är det inte kritiskt för STEP 2, men vi loggar.
    modemSetCfun(0, 5000UL);
    return true;
}

bool modemRfOn()
{
    logSystem("MODEM: RF ON (CFUN=1)");
    modemSetCfun(1, 5000UL);
    return true;
}

void modemPowerCycle(uint32_t offMs, uint32_t bootMs)
{
    logSystem("MODEM: power cycle start");

    // Försök RF off först (inte kritiskt om det failar)
    modemSetCfun(0, 5000UL);

    // PWRKEY-sekvens: din kod använder LOW->HIGH->LOW som “pulse”.
    // Vi kör en längre puls för att trigga power toggle.
    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);

    logSystem("MODEM: PWRKEY long pulse (toggle power)");
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(1500);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);

    // Vänta en stund “off”
    delay(offMs);

    // Starta igen med samma puls
    logSystem("MODEM: PWRKEY pulse (power on)");
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(1200);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);

    // Låt modemet boota innan vi provar AT
    delay(bootMs);

    logSystem("MODEM: power cycle done");
}
