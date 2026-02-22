// profiles.h
#pragma once
#include <Arduino.h>

enum class ProfileId
{
  TRAVEL,
  PARKED,
  ALARM,
  STOLEN
};

struct ProfileConfig
{
  ProfileId id;
  const char *name;        // "TRAVEL", "PARKED", ...
  uint32_t gpsIntervalMs;  // GPS sampling (TRAVEL: 10s, annars typ 5 min)
  uint32_t commIntervalMs; // Comm window (MQTT uplink) intervall
  uint32_t gpsFixWaitMs;   // Hur länge vi max väntar på en bra fix (single-fix)

  bool pirFront; // PIR fram aktiv?
  bool pirBack;  // PIR bak aktiv?
};

void profilesInit(ProfileId defaultProfile);
const ProfileConfig &currentProfile();
void setProfile(ProfileId id);

const char *profileName(ProfileId id);
bool profileFromString(const String &s, ProfileId &out);

extern void pipelineOnProfileChanged(ProfileId newProfile);
