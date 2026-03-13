#include <Arduino.h>

#include "config.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "ext_gnss.h"
#include "profiles.h"
#include "time_manager.h"
#include "logging.h"
#include "pipeline.h"

void setup()
{
  // Starta seriell debug/loggning
  Serial.begin(115200);

  // Ge terminalen tid att ansluta efter boot/reset
  delay(2000);
  Serial.println("=== Campervanlarm – PIPELINE branch ===");

  // Initiera loggsystemet tidigt så att resten av uppstarten kan loggas
  loggingInit();
  logSystem("BOOT: terminal logging only");

  // Initiera tidshantering och defaultprofil
  timeInit();
  profilesInit(ProfileId::PARKED);

  // Initiera PMU och nödvändig matning
  if (!powerInit())
  {
    logSystem("BOOT: powerInit FAILED");
  }

  // Initiera modemets UART/GPIO och MQTT-lagret
  modemInitUartAndPins();
  mqttSetup();

  // Initiera huvudlogiken/state machine
  pipelineInit();
  logSystem("BOOT: pipelineInit done");
}

void loop()
{
  // Kör huvudlogiken
  pipelineTick(millis());
}