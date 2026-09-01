// #region MODULE_CONTRACT
// PURPOSE: Preserves ZTE settings so recovery can clear them independently.
// SCOPE:
// - ZTE profile load/save and V3→V4 migration.
// - NOT: Other profiles, network connections, or protocol dialogs.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_ZTE_H
#define PERSISTENCE_CONFIG_STORE_ZTE_H

#include <Arduino.h>

#include "zte/zte_record.h"

// #region STRUCT_RuntimeZteConfig
// PURPOSE: Carries validated ZTE settings between web, runtime, and NVS boundaries.
struct RuntimeZteConfig {
  bool moduleEnabled = false;
  bool forwardEnabled = false;
  String host;
  String password;
  String label;  // Phone number or alias shown in forwarded emails.
  uint16_t pollIntervalSec = kDefaultZtePollSec;
};
// #endregion STRUCT_RuntimeZteConfig

// #region FUNC_buildZteConfigRecord
// PURPOSE: Gives persistence one bounded representation of ZTE settings.
ZteConfigRecord buildZteConfigRecord(const RuntimeZteConfig& config);
// #endregion FUNC_buildZteConfigRecord

// #region CLASS_ZteConfigStore
// PURPOSE: Owns atomic persistence of ZTE settings in the recovery-safe partition.
class ZteConfigStore {
 public:
  // #region METHOD_ZteConfigStore_load
  // PURPOSE: Keeps corrupt or partial ZTE policy out of the modem dialog.
  bool load(RuntimeZteConfig& config) const;
  // #endregion METHOD_ZteConfigStore_load

  // #region METHOD_ZteConfigStore_save
  // PURPOSE: Keeps rebooted ZTE policy valid and recoverable.
  bool save(const RuntimeZteConfig& config) const;
  // #endregion METHOD_ZteConfigStore_save

 private:
  bool migrateV3Record(size_t readLength) const;
};
// #endregion CLASS_ZteConfigStore
#endif  // PERSISTENCE_CONFIG_STORE_ZTE_H
