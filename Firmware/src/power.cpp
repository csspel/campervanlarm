#include "power.h"
#include "config.h"
#include "logging.h"

#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// Global PMU-instans för AXP2101
// Används för att styra spänningsutgångar på T-SIM7080G-S3-kortet.
static XPowersPMU PMU;

bool powerInit()
{
  // Logga att vi startar initiering av PMU
  logSystem("PMU: init...");

  // Starta kommunikation med AXP2101 via I2C
  // BOARD_I2C_SDA och BOARD_I2C_SCL kommer från config.h
  // AXP2101_SLAVE_ADDRESS är I2C-adressen till PMU:n
  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, BOARD_I2C_SDA, BOARD_I2C_SCL))
  {
    logSystem("PMU: FAILED to initialize");
    return false;
  }

  // =========================================================
  // MODEMMATNING
  // =========================================================
  //
  // DC3 används som huvudmatning till SIM7080-modemet på denna hårdvara.
  // 3000 mV brukar vara en vanlig nivå i exempel för T-SIM7080G-S3.
  // Om modemet senare visar sig instabilt kan man behöva verifiera
  // exakt lämplig spänning mot kortets schema eller tidigare fungerande kod.
  //
  PMU.setDC3Voltage(3000);
  PMU.enableDC3();

  // =========================================================
  // OANVÄNDA FUNKTIONER STÄNGS AV
  // =========================================================
  //
  // Du använder inte internt SD-kort:
  // ALDO3 var tidigare satt till 3.3 V för SD-kortets matning.
  // Vi stänger därför av den utgången för att spara ström
  // och göra beteendet tydligare.
  //
  PMU.disableALDO3();

  //
  // Du använder inte intern GNSS/GPS:
  // BLDO2 var tidigare tänkt för GPS-antenn/GNSS-del.
  // Vi stänger därför av den helt.
  //
  PMU.disableBLDO2();

  // =========================================================
  // TS / NTC-MÄTNING
  // =========================================================
  //
  // TS-pinnen används ofta för batteritemperatur/NTC.
  // Om ingen NTC är ansluten kan mätningen störa eller vara meningslös,
  // så vi stänger av den.
  //
  PMU.disableTSPinMeasure();

  // Logga att nödvändiga matningar nu är igång
  logSystem("PMU: modem power rail ON (DC3), unused rails OFF");

  return true;
}