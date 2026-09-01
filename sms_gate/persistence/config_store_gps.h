// #region MODULE_CONTRACT
// PURPOSE: Preserves GNSS settings so recovery can clear them independently.
// SCOPE:
// - load/save of GpsConfigStore profile and buildGpsRecord helper.
// - NOT: Wi-Fi, SMTP, ZTE and modem-source profiles, network connections and GNSS AT dialogs.
// INVARIANTS: Profile stored as whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_GPS_H
#define PERSISTENCE_CONFIG_STORE_GPS_H

#include <Arduino.h>

#include "gps/gps_record.h"

// #region STRUCT_RuntimeGpsConfig
// PURPOSE: Carries validated GNSS settings between web, runtime, and NVS boundaries.
struct RuntimeGpsConfig {
  bool moduleEnabled = false;
  bool pollEnabled = false;
  uint16_t pollIntervalSec = kDefaultGpsPollSec;
  bool timeSyncEnabled = true;
};
// #endregion STRUCT_RuntimeGpsConfig

// #region FUNC_buildGpsRecord
// PURPOSE: Gives persistence one bounded representation of GNSS settings.
GpsRecord buildGpsRecord(const RuntimeGpsConfig& config);
// #endregion FUNC_buildGpsRecord

// #region CLASS_GpsConfigStore
// PURPOSE: Owns atomic persistence of GNSS settings in the recovery-safe partition.
class GpsConfigStore {
 public:
  // #region METHOD_GpsConfigStore_load
  // PURPOSE: Keeps corrupt or partial GNSS settings out of polling.
  bool load(RuntimeGpsConfig& config) const;
  // #endregion METHOD_GpsConfigStore_load

  // #region METHOD_GpsConfigStore_save
  // PURPOSE: Keeps rebooted GNSS policy valid and recoverable.
  bool save(const RuntimeGpsConfig& config) const;
  // #endregion METHOD_GpsConfigStore_save

 private:
  bool migrateV2Record(size_t readLength) const;
};
// #endregion CLASS_GpsConfigStore

#endif  // PERSISTENCE_CONFIG_STORE_GPS_H
