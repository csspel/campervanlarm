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

void setup()
{
  Serial.begin(115200);

  delay(2000);
  Serial.println("=== Campervanlarm – PIPELINE branch ===");

  timeInit();
  profilesInit(ProfileId::PARKED); // eller det du vill som default

  powerInit();
  loggingInit();
  logSystem("BOOT: terminal logging only");

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
