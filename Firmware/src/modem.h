#pragma once
#include <Arduino.h>
#include <Client.h>

struct NetResult {
    String ip;
    int    csq;
    String err;
};

// Initiera UART + GPIO mot modem (kallas i INIT_HW)
void modemInitUartAndPins();

// Öppna datalänk (NET_CONNECT-state använder denna)
bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out);

// MQTT-klient (TinyGSMClient-wrapper)
Client& modemGetClient();

// Läs modemets klocka via AT+CCLK?  -> returnerar råsträng "yy/MM/dd,hh:mm:ss±zz"
bool modemGetCclk(String &outCclk, uint32_t timeoutMs = 1500);

bool modemRfOn();
bool modemRfOff();

void modemPowerCycle(uint32_t offMs = 3000, uint32_t bootMs = 8000);

