#include <Arduino.h>
#include "config.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "gps.h"
#include "profiles.h"
#include "time_manager.h"
#include "logging.h"
#include "pipeline.h"
#include "esp_log.h"

void setup()
{
  Serial.begin(115200);

  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
  esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);

  delay(2000);
  Serial.println("=== Campervanlarm – PIPELINE branch ===");

  timeInit();
  profilesInit(ProfileId::PARKED); // eller det du vill som default

  powerInit();
  loggingInit();
  logSystem("BOOT: continuing without SD if unavailable");

  modemInitUartAndPins();
  mqttSetup();

  pipelineInit();
  logSystem("BOOT: pipelineInit done");
}

void loop()
{
  pipelineTick(millis());
  // inget annat här – håll main superläsbar
}
