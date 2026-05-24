/**
 * VictronBLE - ESP32 library for Victron Energy BLE devices
 * Implementation file
 *
 * Copyright (c) 2025 Scott Penrose
 * License: MIT
 */

#include "VictronBLE.h"
#include "config.h"
#include <string.h>
#include "esp_log.h"

// Kort diagnostik utan att slå på full Arduino BLE-debug.
// Full BLE-debug kan generera enorm loggmängd och trigga watchdog.
static uint32_t s_victronAdvSeen = 0;
static uint32_t s_victronKnownSeen = 0;
static uint32_t s_victronUnknownSeen = 0;
static uint32_t s_victronParseFailSeen = 0;
static uint32_t s_victronParseSuccessSeen = 0;
static uint32_t s_victronUnknownLogged = 0;
static uint32_t s_victronParseFailLogged = 0;

static void bytesToHexLower(const uint8_t *data, size_t len, char *out, size_t outLen)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || outLen == 0)
        return;
    size_t n = 0;
    for (size_t i = 0; i < len && (n + 2) < outLen; ++i)
    {
        out[n++] = hex[(data[i] >> 4) & 0x0F];
        out[n++] = hex[data[i] & 0x0F];
    }
    out[n] = '\0';
}

VictronBLE::VictronBLE()
    : deviceCount(0), pBLEScan(nullptr), scanCallbackObj(nullptr),
      callback(nullptr), debugEnabled(false), scanDuration(5),
      minIntervalMs(1000), initialized(false)
{
    memset(devices, 0, sizeof(devices));
}

bool VictronBLE::begin(uint32_t scanDuration)
{
    if (initialized)
        return true;
    this->scanDuration = scanDuration;

    // Sänk BLE/BT-loggning. Arduino BLE kan annars spamma Serial så hårt
    // att BTC_TASK svälter IDLE task och watchdog startar om ESP32-S3.
    esp_log_level_set("BLEAdvertisedDevice", ESP_LOG_NONE);
    esp_log_level_set("BLEDevice", ESP_LOG_WARN);
    esp_log_level_set("BLEScan", ESP_LOG_WARN);
    esp_log_level_set("BT_BTM", ESP_LOG_WARN);

    BLEDevice::init("VictronBLE");
    pBLEScan = BLEDevice::getScan();
    if (!pBLEScan)
        return false;

    scanCallbackObj = new VictronBLEAdvertisedDeviceCallbacks(this);
    // false = inga dubbletter under samma scan. Det minskar callback-belastningen
    // kraftigt och räcker för vår periodiska statusläsning.
    pBLEScan->setAdvertisedDeviceCallbacks(scanCallbackObj, true);
    pBLEScan->setActiveScan(false);
    // Låg duty-cycle scan: mindre CPU/radiobelastning än 99/100.
    // duplicate=true är medvetet: Victron kan sända flera advertising frames
    // från samma adress och den första behöver inte vara e102-data.
    pBLEScan->setInterval(160);
    pBLEScan->setWindow(40);

    initialized = true;
    if (debugEnabled)
        Serial.println("[VictronBLE] Initialized");
    return true;
}


bool VictronBLE::scanOnce(uint32_t scanDurationSeconds)
{
    if (!initialized)
        return false;
    if (!pBLEScan)
        return false;

    if (scanDurationSeconds == 0)
        scanDurationSeconds = scanDuration;

    pBLEScan->clearResults();

    if (debugEnabled)
        Serial.printf("[VictronBLE] Blocking scan start: %lus\n", (unsigned long)scanDurationSeconds);

    // ESP32 BLE Arduino har en blockande overload: start(duration, is_continue).
    // Resultatet används inte; dekodning sker i onResult-callbacken.
    pBLEScan->start(scanDurationSeconds, false);
    delay(20);
    // start(duration, false) är blockande och stoppar scanningen själv.
    // Att anropa stop() direkt efteråt gav ofta BT_BTM "scan not active".
    pBLEScan->clearResults();

    if (debugEnabled)
        Serial.println("[VictronBLE] Blocking scan done");

    return true;
}

void VictronBLE::end()
{
    if (pBLEScan)
    {
        // Stoppa inte ovillkorligt här. Vid blockande scan är den redan stoppad
        // och ESP32 BT-stacken loggar annars "scan not active".
        pBLEScan->clearResults();
    }

    if (scanCallbackObj)
    {
        delete scanCallbackObj;
        scanCallbackObj = nullptr;
    }

    pBLEScan = nullptr;
    initialized = false;

    // Frigör BLE-minne/radioresurser mellan scans.
    BLEDevice::deinit(true);
}

void VictronBLE::resetScanStats()
{
    s_victronAdvSeen = 0;
    s_victronKnownSeen = 0;
    s_victronUnknownSeen = 0;
    s_victronParseFailSeen = 0;
    s_victronParseSuccessSeen = 0;
    s_victronUnknownLogged = 0;
    s_victronParseFailLogged = 0;
}

uint32_t VictronBLE::getScanAdvSeen() const { return s_victronAdvSeen; }
uint32_t VictronBLE::getScanKnownSeen() const { return s_victronKnownSeen; }
uint32_t VictronBLE::getScanUnknownSeen() const { return s_victronUnknownSeen; }
uint32_t VictronBLE::getScanParseFailSeen() const { return s_victronParseFailSeen; }
uint32_t VictronBLE::getScanParseSuccessSeen() const { return s_victronParseSuccessSeen; }

bool VictronBLE::addDevice(const char *name, const char *mac, const char *hexKey,
                           VictronDeviceType type)
{
    if (deviceCount >= VICTRON_MAX_DEVICES)
        return false;
    if (!hexKey || strlen(hexKey) != 32)
        return false;
    if (!mac || strlen(mac) == 0)
        return false;

    char normalizedMAC[VICTRON_MAC_LEN];
    normalizeMAC(mac, normalizedMAC);

    // Check for duplicate
    if (findDevice(normalizedMAC))
        return false;

    DeviceEntry *entry = &devices[deviceCount];
    memset(entry, 0, sizeof(DeviceEntry));
    entry->active = true;

    strncpy(entry->device.name, name ? name : "", VICTRON_NAME_LEN - 1);
    entry->device.name[VICTRON_NAME_LEN - 1] = '\0';
    memcpy(entry->device.mac, normalizedMAC, VICTRON_MAC_LEN);
    entry->device.deviceType = type;
    entry->device.rssi = -100;

    if (!hexToBytes(hexKey, entry->key, 16))
        return false;

    deviceCount++;

    if (debugEnabled)
        Serial.printf("[VictronBLE] Added: %s (%s)\n", name, normalizedMAC);
    return true;
}

// Scan complete callback — sets flag so loop() restarts
static bool s_scanning = false;
static void onScanDone(BLEScanResults results)
{
    s_scanning = false;
}

void VictronBLE::loop()
{
    if (!initialized)
        return;
    if (!s_scanning)
    {
        pBLEScan->clearResults();
        s_scanning = pBLEScan->start(scanDuration, onScanDone, false);
    }
}

// BLE scan callback
void VictronBLEAdvertisedDeviceCallbacks::onResult(BLEAdvertisedDevice advertisedDevice)
{
    if (victronBLE)
        victronBLE->processDevice(advertisedDevice);
}

void VictronBLE::processDevice(BLEAdvertisedDevice &advertisedDevice)
{
    if (!advertisedDevice.haveManufacturerData())
        return;

    std::string raw = advertisedDevice.getManufacturerData();
    if (raw.length() < 10)
        return;

    // Quick vendor ID check before any other work
    uint16_t vendorID = (uint8_t)raw[0] | ((uint8_t)raw[1] << 8);
    if (vendorID != VICTRON_MANUFACTURER_ID)
        return;

    // Parse manufacturer data
    victronManufacturerData mfgData;
    memset(&mfgData, 0, sizeof(mfgData));
    size_t copyLen = raw.length() > sizeof(mfgData) ? sizeof(mfgData) : raw.length();
    raw.copy(reinterpret_cast<char *>(&mfgData), copyLen);

    // Normalize MAC and find device
    char normalizedMAC[VICTRON_MAC_LEN];
    normalizeMAC(advertisedDevice.getAddress().toString().c_str(), normalizedMAC);

    s_victronAdvSeen++;

    DeviceEntry *entry = findDevice(normalizedMAC);
    if (!entry)
    {
        s_victronUnknownSeen++;
        if (VICTRON_BLE_DIAG_VERBOSE && s_victronUnknownLogged < VICTRON_BLE_DIAG_MAX_UNKNOWN_PER_SCAN)
        {
            char rawHex[65];
            bytesToHexLower(reinterpret_cast<const uint8_t *>(raw.data()), raw.length(), rawHex, sizeof(rawHex));
            Serial.printf("VICTRON_DIAG: unknown_victron_mac=%s rssi=%d len=%u beacon=0x%02X record=0x%02X keymatch=0x%02X nonce=0x%04X raw=%s seen=%lu\n",
                          normalizedMAC,
                          advertisedDevice.getRSSI(),
                          (unsigned)raw.length(),
                          mfgData.beaconType,
                          mfgData.victronRecordType,
                          mfgData.encryptKeyMatch,
                          mfgData.nonceDataCounter,
                          rawHex,
                          (unsigned long)s_victronAdvSeen);
            s_victronUnknownLogged++;
        }
        return;
    }

    s_victronKnownSeen++;

    // Skip if nonce unchanged (data hasn't changed on the device)
    if (entry->device.dataValid && mfgData.nonceDataCounter == entry->lastNonce)
    {
        // Still update RSSI since we got a packet
        entry->device.rssi = advertisedDevice.getRSSI();
        return;
    }

    // Skip if minimum interval hasn't elapsed
    uint32_t now = millis();
    if (entry->device.dataValid && (now - entry->device.lastUpdate) < minIntervalMs)
    {
        return;
    }

    if (debugEnabled)
        Serial.printf("[VictronBLE] Processing: %s nonce:0x%04X\n",
                      entry->device.name, mfgData.nonceDataCounter);

    if (parseAdvertisement(entry, mfgData))
    {
        s_victronParseSuccessSeen++;
        entry->lastNonce = mfgData.nonceDataCounter;
        entry->device.rssi = advertisedDevice.getRSSI();
        entry->device.lastUpdate = now;
    }
    else
    {
        s_victronParseFailSeen++;
        if (VICTRON_BLE_DIAG_VERBOSE && s_victronParseFailLogged < VICTRON_BLE_DIAG_MAX_PARSE_FAIL_PER_SCAN)
        {
        char rawHex[65];
        bytesToHexLower(reinterpret_cast<const uint8_t *>(raw.data()), raw.length(), rawHex, sizeof(rawHex));
        Serial.printf("VICTRON_DIAG: known_parse_fail name=%s mac=%s rssi=%d len=%u beacon=0x%02X record=0x%02X keymatch=0x%02X key0=0x%02X nonce=0x%04X raw=%s known_seen=%lu\n",
                      entry->device.name,
                      normalizedMAC,
                      advertisedDevice.getRSSI(),
                      (unsigned)raw.length(),
                      mfgData.beaconType,
                      mfgData.victronRecordType,
                      mfgData.encryptKeyMatch,
                      entry->key[0],
                      mfgData.nonceDataCounter,
                      rawHex,
                      (unsigned long)s_victronKnownSeen);
        s_victronParseFailLogged++;
        }
    }
}

bool VictronBLE::parseAdvertisement(DeviceEntry *entry, const victronManufacturerData &mfg)
{
    if (debugEnabled)
    {
        Serial.printf("[VictronBLE] Beacon:0x%02X Record:0x%02X Nonce:0x%04X\n",
                      mfg.beaconType, mfg.victronRecordType, mfg.nonceDataCounter);
    }

    // Lägg in här
    if (mfg.beaconType != 0x10)
    {
        return false;
    }

    // Quick key check before expensive decryption
    if (mfg.encryptKeyMatch != entry->key[0])
    {
        if (debugEnabled)
            Serial.println("[VictronBLE] Key byte mismatch");
        return false;
    }

    // Build IV from nonce (2 bytes little-endian + 14 zero bytes)
    uint8_t iv[16] = {0};
    iv[0] = mfg.nonceDataCounter & 0xFF;
    iv[1] = (mfg.nonceDataCounter >> 8) & 0xFF;

    // Decrypt
    uint8_t decrypted[VICTRON_ENCRYPTED_LEN];
    if (!decryptData(mfg.victronEncryptedData, VICTRON_ENCRYPTED_LEN,
                     entry->key, iv, decrypted))
    {
        if (debugEnabled)
            Serial.println("[VictronBLE] Decryption failed");
        return false;
    }

    // Parse based on record type (auto-detects device type)
    bool ok = false;
    switch (mfg.victronRecordType)
    {
    case DEVICE_TYPE_SOLAR_CHARGER:
        entry->device.deviceType = DEVICE_TYPE_SOLAR_CHARGER;
        ok = parseSolarCharger(decrypted, VICTRON_ENCRYPTED_LEN, entry->device.solar);
        break;
    case DEVICE_TYPE_BATTERY_MONITOR:
        entry->device.deviceType = DEVICE_TYPE_BATTERY_MONITOR;
        ok = parseBatteryMonitor(decrypted, VICTRON_ENCRYPTED_LEN, entry->device.battery);
        break;
    case DEVICE_TYPE_INVERTER:
    case DEVICE_TYPE_INVERTER_RS:
    case DEVICE_TYPE_MULTI_RS:
    case DEVICE_TYPE_VE_BUS:
        entry->device.deviceType = DEVICE_TYPE_INVERTER;
        ok = parseInverter(decrypted, VICTRON_ENCRYPTED_LEN, entry->device.inverter);
        break;
    case DEVICE_TYPE_DCDC_CONVERTER:
        entry->device.deviceType = DEVICE_TYPE_DCDC_CONVERTER;
        ok = parseDCDCConverter(decrypted, VICTRON_ENCRYPTED_LEN, entry->device.dcdc);
        break;
    default:
        if (debugEnabled)
            Serial.printf("[VictronBLE] Unknown type: 0x%02X\n", mfg.victronRecordType);
        return false;
    }

    if (ok)
    {
        entry->device.dataValid = true;
        if (callback)
            callback(&entry->device);
    }

    return ok;
}

bool VictronBLE::decryptData(const uint8_t *encrypted, size_t len,
                             const uint8_t *key, const uint8_t *iv,
                             uint8_t *decrypted)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    if (mbedtls_aes_setkey_enc(&aes, key, 128) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }

    size_t nc_off = 0;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    memcpy(nonce_counter, iv, 16);
    memset(stream_block, 0, 16);

    int ret = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter,
                                    stream_block, encrypted, decrypted);
    mbedtls_aes_free(&aes);
    return (ret == 0);
}

bool VictronBLE::parseSolarCharger(const uint8_t *data, size_t len, VictronSolarData &result)
{
    if (len < sizeof(victronSolarChargerPayload))
        return false;
    const auto *p = reinterpret_cast<const victronSolarChargerPayload *>(data);

    result.chargeState = p->deviceState;
    result.errorCode = p->errorCode;
    result.batteryVoltage = p->batteryVoltage * 0.01f;
    result.batteryCurrent = p->batteryCurrent * 0.01f;
    result.yieldToday = p->yieldToday * 10;
    result.panelPower = p->inputPower;
    result.loadCurrent = (p->loadCurrent != 0xFFFF) ? p->loadCurrent * 0.01f : 0;

    if (debugEnabled)
    {
        Serial.printf("[VictronBLE] Solar: %.2fV %.2fA %dW State:%d\n",
                      result.batteryVoltage, result.batteryCurrent,
                      (int)result.panelPower, result.chargeState);
    }
    return true;
}

bool VictronBLE::parseBatteryMonitor(const uint8_t *data, size_t len, VictronBatteryData &result)
{
    if (len < 15)
        return false;

    if (debugEnabled)
    {
        Serial.print("[VictronBLE] Battery raw decrypted: ");
        for (size_t i = 0; i < len; i++)
        {
            if (data[i] < 16)
                Serial.print("0");
            Serial.print(data[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
    }

    result.remainingMinutes = data[0] | (data[1] << 8);
    result.voltage = (data[2] | (data[3] << 8)) * 0.01f;

    uint16_t alarmReason = data[4] | (data[5] << 8);

    result.alarmLowVoltage = (alarmReason & 0x0001) != 0;
    result.alarmHighVoltage = (alarmReason & 0x0002) != 0;
    result.alarmLowSOC = (alarmReason & 0x0004) != 0;
    result.alarmLowTemperature = (alarmReason & 0x0020) != 0;
    result.alarmHighTemperature = (alarmReason & 0x0040) != 0;

    uint16_t auxData = data[6] | (data[7] << 8);

    // Bitfältet börjar vid byte 8.
    // Layout enligt ESPHome/Victron:
    // aux_input_type : 2 bit
    // battery_current : 22 bit signed, 0.001 A
    // consumed_ah : 20 bit unsigned, 0.1 Ah, visas som negativt
    // state_of_charge : 10 bit unsigned, 0.1 %
    uint64_t bits = 0;
    for (int i = 0; i < 7; i++)
    {
        bits |= ((uint64_t)data[8 + i]) << (8 * i);
    }

    uint8_t auxInputType = bits & 0x03;

    int32_t currentRaw = (bits >> 2) & 0x3FFFFF;
    if (currentRaw & 0x200000)
    {
        currentRaw |= 0xFFC00000;
    }
    result.current = currentRaw * 0.001f;

    uint32_t consumedRaw = (bits >> 24) & 0xFFFFF;
    result.consumedAh = -((float)consumedRaw * 0.1f);

    uint16_t socRaw = (bits >> 44) & 0x03FF;
    result.soc = socRaw * 0.1f;

    // Aux type:
    // 0 = starter/aux voltage
    // 1 = midpoint voltage
    // 2 = temperature
    // 3 = none
    result.auxVoltage = 0;
    result.temperature = 0;

    if (auxInputType == 0 || auxInputType == 1)
    {
        result.auxVoltage = auxData * 0.01f;
    }
    else if (auxInputType == 2)
    {
        result.temperature = (auxData * 0.01f) - 273.15f;
    }

    if (debugEnabled)
    {
        Serial.printf("[VictronBLE] Battery: %.2fV %.3fA SOC:%.1f%% Consumed:%.2fAh Togo:%umin AuxType:%u\n",
                      result.voltage,
                      result.current,
                      result.soc,
                      result.consumedAh,
                      result.remainingMinutes,
                      auxInputType);
    }

    return true;
}

bool VictronBLE::parseInverter(const uint8_t *data, size_t len, VictronInverterData &result)
{
    if (len < sizeof(victronInverterPayload))
        return false;
    const auto *p = reinterpret_cast<const victronInverterPayload *>(data);

    result.state = p->deviceState;
    result.batteryVoltage = p->batteryVoltage * 0.01f;
    result.batteryCurrent = p->batteryCurrent * 0.01f;

    // AC Power (signed 24-bit)
    int32_t acPower = p->acPowerLow | (p->acPowerMid << 8) | (p->acPowerHigh << 16);
    if (acPower & 0x800000)
        acPower |= 0xFF000000; // Sign extend
    result.acPower = acPower;

    // Alarm bits
    result.alarmLowVoltage = (p->alarms & 0x01) != 0;
    result.alarmHighVoltage = (p->alarms & 0x02) != 0;
    result.alarmHighTemperature = (p->alarms & 0x04) != 0;
    result.alarmOverload = (p->alarms & 0x08) != 0;

    if (debugEnabled)
    {
        Serial.printf("[VictronBLE] Inverter: %.2fV %dW State:%d\n",
                      result.batteryVoltage, (int)result.acPower, result.state);
    }
    return true;
}

bool VictronBLE::parseDCDCConverter(const uint8_t *data, size_t len, VictronDCDCData &result)
{
    if (len < sizeof(victronDCDCConverterPayload))
        return false;
    const auto *p = reinterpret_cast<const victronDCDCConverterPayload *>(data);

    result.chargeState = p->chargeState;
    result.errorCode = p->errorCode;
    result.inputVoltage = p->inputVoltage * 0.01f;
    result.outputVoltage = p->outputVoltage * 0.01f;
    result.outputCurrent = p->outputCurrent * 0.01f;

    if (debugEnabled)
    {
        Serial.printf("[VictronBLE] DC-DC: In=%.2fV Out=%.2fV %.2fA\n",
                      result.inputVoltage, result.outputVoltage, result.outputCurrent);
    }
    return true;
}

// --- Helpers ---

bool VictronBLE::hexToBytes(const char *hex, uint8_t *out, size_t len)
{
    if (strlen(hex) != len * 2)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t hi = hex[i * 2], lo = hex[i * 2 + 1];
        if (hi >= '0' && hi <= '9')
            hi -= '0';
        else if (hi >= 'a' && hi <= 'f')
            hi = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F')
            hi = hi - 'A' + 10;
        else
            return false;
        if (lo >= '0' && lo <= '9')
            lo -= '0';
        else if (lo >= 'a' && lo <= 'f')
            lo = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F')
            lo = lo - 'A' + 10;
        else
            return false;
        out[i] = (hi << 4) | lo;
    }
    return true;
}

void VictronBLE::normalizeMAC(const char *input, char *output)
{
    int j = 0;
    for (int i = 0; input[i] && j < VICTRON_MAC_LEN - 1; i++)
    {
        char c = input[i];
        if (c == ':' || c == '-')
            continue;
        output[j++] = (c >= 'A' && c <= 'F') ? (c + 32) : c;
    }
    output[j] = '\0';
}

VictronBLE::DeviceEntry *VictronBLE::findDevice(const char *normalizedMAC)
{
    for (size_t i = 0; i < deviceCount; i++)
    {
        if (devices[i].active && strcmp(devices[i].device.mac, normalizedMAC) == 0)
        {
            return &devices[i];
        }
    }
    return nullptr;
}
