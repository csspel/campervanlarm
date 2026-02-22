// profiles.cpp
#include "profiles.h"

// En enda källa för alla intervall.
// Justera här – så följer main.cpp automatiskt med.
static ProfileConfig profileTable[] = {
    // TRAVEL: GPS ofta (men uplink avgör hur ofta du faktiskt skickar), PIR normalt AV i travel
    {ProfileId::TRAVEL, "TRAVEL",
     10 * 1000UL,     // gpsIntervalMs (används inte fullt ut i pipeline just nu)
     5 * 60 * 1000UL, // commIntervalMs (styr i praktiken när GPS tas/skickas)
     30 * 1000UL,     // gpsFixWaitMs  (MÅSTE vara >0 annars blir det ingen GPS)
     false, false},   // pirFront, pirBack

    // PARKED: GPS var 5 min, PIR på (om du vill övervaka i parked)
    {ProfileId::PARKED, "PARKED",
     5 * 60 * 1000UL,
     5 * 60 * 1000UL,
     60 * 1000UL,
     true, true},

    // ALARM: GPS som backup + PIR på
    {ProfileId::ALARM, "ALARM",
     5 * 60 * 1000UL,
     5 * 60 * 1000UL,
     60 * 1000UL,
     true, true},

    // STOLEN: tätare spårning, PIR kan vara vad du vill (jag sätter av här)
    {ProfileId::STOLEN, "STOLEN",
     2 * 60 * 1000UL,
     2 * 60 * 1000UL,
     60 * 1000UL,
     false, false},
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
  extern void pipelineOnProfileChanged(ProfileId newProfile);
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

  if (up == "TRAVEL")
  {
    out = ProfileId::TRAVEL;
    return true;
  }
  if (up == "PARKED")
  {
    out = ProfileId::PARKED;
    return true;
  }
  if (up == "ALARM")
  {
    out = ProfileId::ALARM;
    return true;
  }
  if (up == "STOLEN")
  {
    out = ProfileId::STOLEN;
    return true;
  }
  return false;
}
