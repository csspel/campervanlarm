#pragma once

#include <Arduino.h>
#include <Client.h>

// Resultat från nät/data-uppkoppling.
struct NetResult
{
    String ip;  // Tilldelad lokal IP-adress
    int csq;    // Signalstyrka enligt CSQ
    String err; // Felkod/text vid misslyckande, tom sträng vid OK
};

// Initierar UART och GPIO mot modemet.
// Bör anropas tidigt i hårdvaruinit.
void modemInitUartAndPins();

// Ansluter modemet till mobilnätet och aktiverar data.
// Parametrar:
// - apn: operatörens APN
// - netRegTimeoutMs: timeout för nätregistrering
// - dataAttachTimeoutMs: timeout för dataaktivering
// - out: fylls med IP, CSQ och ev. felinfo
//
// Returnerar true vid lyckad datauppkoppling.
bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out);

// Returnerar den TCP/IP-klient som går via modemet.
// Används t.ex. av MQTT-klienten.
Client &modemGetClient();

// Läser modemets klocka via AT+CCLK?
// Returnerar råsträngen i format:
//   "yy/MM/dd,hh:mm:ss±zz"
bool modemGetCclk(String &outCclk, uint32_t timeoutMs = 1500);

// Slår på radiofunktionen (CFUN=1).
// Returnerar true om kommandot lyckades.
bool modemRfOn();

// Slår av radiofunktionen (CFUN=0).
// Returnerar true om kommandot lyckades.
bool modemRfOff();

// Gör en full power-cycle av modemet via PWRKEY.
// offMs  = väntetid mellan av/på
// bootMs = väntetid för uppstart innan modemet antas redo
void modemPowerCycle(uint32_t offMs = 3000, uint32_t bootMs = 8000);