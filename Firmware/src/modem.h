#pragma once

#include <Arduino.h>
#include <Client.h>

// Resultat från nät/data-uppkoppling.
struct NetResult
{
    String ip;              // Tilldelad lokal IP-adress
    int csq;                // Signalstyrka enligt CSQ
    String err;             // Felkod/text vid misslyckande, tom sträng vid OK
    uint32_t connectMs = 0; // Total uppkopplingstid i ms
};

// Status för icke-blockerande uppkopplingsförsök.
enum class ModemConnectState
{
    IDLE = 0,
    WAIT_AT,
    CONFIGURE_RADIO,
    RF_SETTLE,
    WAIT_NET_FIRST,
    RF_RESTART_OFF,
    RF_RESTART_PAUSE,
    RF_RESTART_RECONFIGURE,
    RF_RESTART_SETTLE,
    WAIT_NET_FALLBACK,
    ACTIVATE_DATA,
    READ_STATUS,
    DONE_OK,
    DONE_FAIL
};

// Initierar UART och GPIO mot modemet.
void modemInitUartAndPins();

// ------------------------------------------------------------
// Ny icke-blockerande uppkopplingsmodell
// ------------------------------------------------------------

// Starta ett nytt connect-försök.
void modemStartConnectData(const char *apn,
                           uint32_t netRegTimeoutMs,
                           uint32_t dataAttachTimeoutMs);

// Ticka den pågående state-machinen.
// Returnerar true när försöket är färdigt, antingen OK eller FAIL.
// success sätts då till true/false och out innehåller resultat.
bool modemTickConnectData(NetResult &out, bool &success);

// Avbryt pågående connect-försök och återgå till IDLE.
void modemAbortConnectData();

// Returnerar true om ett connect-försök pågår.
bool modemIsConnectBusy();

// Valfritt: läs nuvarande state för logg/debug.
ModemConnectState modemGetConnectState();

// ------------------------------------------------------------
// Gammal blockerande funktion
// Behålls tills pipeline är ombyggd.
// ------------------------------------------------------------
bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out);

// Returnerar den TCP/IP-klient som går via modemet.
Client &modemGetClient();

// Läser aktuell signalstyrka från modemet enligt CSQ.
// Returnerar -1 om värdet inte kunde läsas.
int modemGetSignalQuality();

// Läser modemets klocka via AT+CCLK?
bool modemGetCclk(String &outCclk, uint32_t timeoutMs = 1500);

// Slår på radiofunktionen (CFUN=1).
bool modemRfOn();

// Slår av radiofunktionen (CFUN=0).
bool modemRfOff();

// Gör en full power-cycle av modemet via PWRKEY.
void modemPowerCycle(uint32_t offMs = 3000, uint32_t bootMs = 8000);