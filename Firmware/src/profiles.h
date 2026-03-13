#pragma once

#include <Arduino.h>
#include <stdint.h>

// ============================================================
// Profil-ID
// ------------------------------------------------------------
// PARKED   : Parkerad, ej larmad
// TRAVEL   : Körläge / resa
// ARMED    : Larmad men inte utlöst
// TRIGGERED: Lokalt utlöst av PIR från ARMED
// ALARM    : Externt satt alarm-läge från Home Assistant
// ============================================================
enum class ProfileId
{
  PARKED,
  TRAVEL,
  ARMED,
  TRIGGERED,
  ALARM
};

// ============================================================
// Profilkonfiguration
// ------------------------------------------------------------
// Fält:
// - id: intern identifierare
// - name: namn som används i logg/MQTT
// - commIntervalMs: hur ofta vi vill publicera alive/GPS
// - pirFront: om främre PIR är aktiv i denna profil
// - pirBack: om bakre PIR är aktiv i denna profil
// - keepConnected: om RF/data/MQTT ska hållas uppe mellan cykler
// - autoReturnMs: används för profiler som automatiskt ska gå vidare
//                 till annan profil efter timeout. 0 = ingen auto-return.
//
// I denna modell används autoReturnMs bara av TRIGGERED,
// som automatiskt återgår till ARMED efter timeout.
// ============================================================
struct ProfileConfig
{
  ProfileId id;
  const char *name;
  uint32_t commIntervalMs;
  bool pirFront;
  bool pirBack;
  bool keepConnected;
  uint32_t autoReturnMs;
};

// Initierar aktiv profil vid uppstart.
void profilesInit(ProfileId defaultProfile);

// Returnerar aktuell aktiv profil.
const ProfileConfig &currentProfile();

// Sätter ny profil.
// Om samma profil redan är aktiv görs inget.
void setProfile(ProfileId id);

// Returnerar profilnamn som text.
const char *profileName(ProfileId id);

// Tolkar profil från text, t.ex. från MQTT desired_profile.
// Returnerar true om strängen matchar en känd profil.
bool profileFromString(const String &s, ProfileId &out);

// Hook som pipeline använder för att få veta när profil ändrats.
extern void pipelineOnProfileChanged(ProfileId newProfile);