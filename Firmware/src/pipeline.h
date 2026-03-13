#pragma once

#include <stdint.h>
#include "profiles.h"

// Initierar pipeline/state machine.
// Anropas en gång i setup().
void pipelineInit();

// Kör pipeline/state machine.
// Ska anropas ofta från loop().
void pipelineTick(uint32_t nowMs);

// Hook som anropas när ett PIR-event blivit kvitterat från HA/server.
void pipelineOnPirAck(uint32_t eventId);

// Hook som anropas när aktiv profil ändras.
void pipelineOnProfileChanged(ProfileId newProfile);