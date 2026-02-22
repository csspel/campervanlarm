#pragma once
#include <Arduino.h>
#include "profiles.h"
#include "gps.h"

// Initieras i setup()
void pipelineInit();

// Körs ofta i loop()
void pipelineTick(uint32_t nowMs);

// Hooks (anropas från mqtt.cpp och profiles.cpp)
void pipelineOnPirAck(uint32_t eventId);
void pipelineOnProfileChanged(ProfileId newProfile);
