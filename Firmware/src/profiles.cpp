// profiles.cpp
#include "profiles.h"

// Intervall enligt krav/spec:
// PARKED: alive 5 min + single GPS vid varje alive (GNSS “on” hanteras i pipeline sen)
// TRAVEL: batch 5 min (30x10s) + alive 5 min -> gpsInterval=10s, commInterval=5min
// TRIGGERED: single 15s + alive 15s
// ARMED: alive 30 min (sleep), PIR endast här
static ProfileConfig profileTable[] = {
    // PARKED (disarmed)
    {
        ProfileId::PARKED, "PARKED",
        5 * 60 * 1000UL, // gpsIntervalMs (single vid alive-cykel)
        5 * 60 * 1000UL, // commIntervalMs
        0 * 60 * 1000UL, // gpsFixWaitMs (per cykel, kan justeras senare)
        false, false     // PIR OFF
    },

    // TRAVEL (disarmed)
    {
        ProfileId::TRAVEL, "TRAVEL",
        0 * 1000UL,      // var 10 gpsIntervalMs (sample för batch, dt=10s)
        5 * 60 * 1000UL, // commIntervalMs (batch + alive var 5 min)
        0 * 30 * 1000UL, // gpsFixWaitMs
        false, false     // PIR OFF
    },

    // ARMED (sensorer aktiva)
    {
        ProfileId::ARMED, "ARMED",
        30 * 60 * 1000UL, // gpsIntervalMs (om du ens tar GPS här - kan vara samma som comm)
        30 * 60 * 1000UL, // commIntervalMs (alive var 30 min)
        0 * 60 * 1000UL,  // gpsFixWaitMs
        true, true        // PIR ON
    },

    // TRIGGERED (utlöst larm)
    {
        ProfileId::TRIGGERED, "TRIGGERED",
        15 * 1000UL,     // gpsIntervalMs (single var 15s)
        15 * 1000UL,     // commIntervalMs (alive var 15s)
        0 * 15 * 1000UL, // gpsFixWaitMs
        false, false     // PIR OFF (PIR är bara i ARMED)
    },
};

static ProfileId currentId = ProfileId::PARKED;

static const ProfileConfig &findProfile(ProfileId id)
{
  for (auto &p : profileTable)
  {
    if (p.id == id)
      return p;
  }
  return profileTable[0];
}

void profilesInit(ProfileId defaultProfile)
{
  currentId = defaultProfile;
}

const ProfileConfig &currentProfile()
{
  return findProfile(currentId);
}

void setProfile(ProfileId id)
{
  currentId = id;
  pipelineOnProfileChanged(id);
}

const char *profileName(ProfileId id)
{
  return findProfile(id).name;
}

bool profileFromString(const String &s, ProfileId &out)
{
  String up = s;
  up.toUpperCase();

  if (up == "PARKED")
  {
    out = ProfileId::PARKED;
    return true;
  }
  if (up == "TRAVEL")
  {
    out = ProfileId::TRAVEL;
    return true;
  }
  if (up == "ARMED")
  {
    out = ProfileId::ARMED;
    return true;
  }
  if (up == "TRIGGERED")
  {
    out = ProfileId::TRIGGERED;
    return true;
  }

  return false;
}