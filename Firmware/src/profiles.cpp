#include "profiles.h"
#include "logging.h"
#include "pipeline.h"

// ============================================================
// Profiltabell
// ------------------------------------------------------------
// PARKED:
// - Ej larmad
// - RF/MQTT av mellan kommunikationsfönster
// - GPS + alive ungefär var 5:e minut
//
// TRAVEL:
// - Körläge
// - RF/MQTT hålls uppe
// - single GPS + alive var 10:e sekund
//
// ARMED:
// - Larmad men lugnt läge
// - PIR aktiv
// - RF/MQTT av mellan kommunikationsfönster
// - alive/GPS glest
//
// TRIGGERED:
// - Automatiskt lokalt läge när PIR triggar i ARMED
// - RF/MQTT hålls uppe
// - tätare kommunikation
// - återgår automatiskt till ARMED efter autoReturnMs
//
// ALARM:
// - Externt satt alarm-läge från Home Assistant
// - RF/MQTT hålls uppe
// - tät kommunikation
// - ingen auto-return här
// ============================================================
static const ProfileConfig profileTable[] = {
    // PARKED
    {
        ProfileId::PARKED,
        "PARKED",
        5UL * 60UL * 1000UL, // commIntervalMs = 5 min
        false,               // pirFront
        false,               // pirBack
        false,               // keepConnected
        0                    // autoReturnMs
    },

    // TRAVEL
    {
        ProfileId::TRAVEL,
        "TRAVEL",
        10UL * 1000UL, // commIntervalMs = 10 s
        false,         // pirFront
        false,         // pirBack
        true,          // keepConnected
        0              // autoReturnMs
    },

    // ARMED
    {
        ProfileId::ARMED,
        "ARMED",
        30UL * 60UL * 1000UL, // commIntervalMs = 30 min
        true,                 // pirFront
        true,                 // pirBack
        false,                // keepConnected
        0                     // autoReturnMs
    },

    // TRIGGERED
    {
        ProfileId::TRIGGERED,
        "TRIGGERED",
        15UL * 1000UL,       // commIntervalMs = 15 s
        true,                // pirFront
        true,                // pirBack
        true,                // keepConnected
        30UL * 60UL * 1000UL // autoReturnMs = 30 min tillbaka till ARMED
    },

    // ALARM
    {
        ProfileId::ALARM,
        "ALARM",
        15UL * 1000UL, // commIntervalMs = 15 s
        true,          // pirFront
        true,          // pirBack
        true,          // keepConnected
        0              // autoReturnMs
    },
};

// Aktiv profil i RAM
static ProfileId currentId = ProfileId::PARKED;

// ------------------------------------------------------------
// Intern helper: hitta profil i tabellen.
// Om ingen match hittas returneras första posten som fallback.
// ------------------------------------------------------------
static const ProfileConfig &findProfile(ProfileId id)
{
  for (const auto &p : profileTable)
  {
    if (p.id == id)
    {
      return p;
    }
  }

  return profileTable[0];
}

// Initierar aktiv profil vid uppstart.
void profilesInit(ProfileId defaultProfile)
{
  currentId = defaultProfile;
}

// Returnerar aktuell profilkonfiguration.
const ProfileConfig &currentProfile()
{
  return findProfile(currentId);
}

// Byter profil.
// Om samma profil redan är aktiv görs inget.
void setProfile(ProfileId id)
{
  if (currentId == id)
  {
    return;
  }

  ProfileId oldId = currentId;
  currentId = id;

  logSystem(String("PROFILE: changed from ") +
            profileName(oldId) +
            " to " +
            profileName(id));

  pipelineOnProfileChanged(id);
}

// Returnerar profilnamn som text.
const char *profileName(ProfileId id)
{
  return findProfile(id).name;
}

// Tolkar text till profil.
// Matchning är case-insensitive.
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

  if (up == "ALARM")
  {
    out = ProfileId::ALARM;
    return true;
  }

  return false;
}