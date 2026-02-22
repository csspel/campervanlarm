// profiles.h
#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class ProfileId
{
  PARKED,
  TRAVEL,
  ARMED,
  TRIGGERED
};

struct ProfileConfig
{
  ProfileId id;
  const char *name;        // "PARKED", "TRAVEL", "ARMED", "TRIGGERED"
  uint32_t gpsIntervalMs;  // hur ofta vi tar GPS (single eller sample för batch)
  uint32_t commIntervalMs; // hur ofta vi gör uplink / comm-window
  uint32_t gpsFixWaitMs;   // max “per cykel” att försöka få bra fix
  bool pirFront;           // PIR fram aktiv?
  bool pirBack;            // PIR bak aktiv?
};

void profilesInit(ProfileId defaultProfile);
const ProfileConfig &currentProfile();
void setProfile(ProfileId id);
const char *profileName(ProfileId id);

// Parse från MQTT desired_profile.
// Stöder även gamla namn för bakåtkomp: ALARM->ARMED, STOLEN->TRIGGERED.
bool profileFromString(const String &s, ProfileId &out);

// Hook som pipeline redan använder
extern void pipelineOnProfileChanged(ProfileId newProfile);