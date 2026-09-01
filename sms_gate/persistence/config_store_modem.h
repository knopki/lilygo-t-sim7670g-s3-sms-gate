// #region MODULE_CONTRACT
// PURPOSE: Preserves modem settings so recovery can clear them independently.
// SCOPE:
// - load/save of ModemSourceStore profile and buildModemSourceRecord helper.
// - NOT: Wi-Fi, SMTP, and ZTE profiles, network connection attempts, and protocol dialogs.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_MODEM_H
#define PERSISTENCE_CONFIG_STORE_MODEM_H

#include <Arduino.h>

#include "modem/modem_record.h"

// #region STRUCT_RuntimeModemSourceConfig
// PURPOSE: Carries validated modem-source settings between web, runtime, and NVS boundaries.
struct RuntimeModemSourceConfig {
  bool moduleEnabled = false;
  bool pollEnabled = false;
  uint16_t pollIntervalSec = kDefaultModemPollSec;
  String label;  // Phone number or alias shown in forwarded emails.
  bool nitzTimeSyncEnabled = true;
  bool smsPollEnabled = false;
};
// #endregion STRUCT_RuntimeModemSourceConfig

// #region FUNC_buildModemSourceRecord
// PURPOSE: Gives persistence one bounded representation of modem settings.
ModemSourceRecord buildModemSourceRecord(const RuntimeModemSourceConfig& config);
// #endregion FUNC_buildModemSourceRecord

// #region CLASS_ModemSourceStore
// PURPOSE: Owns atomic persistence of modem-source settings in the recovery-safe partition.
class ModemSourceStore {
 public:
  // #region METHOD_ModemSourceStore_load
  // PURPOSE: Keeps corrupt or partial modem policy out of SMS polling.
  bool load(RuntimeModemSourceConfig& config) const;
  // #endregion METHOD_ModemSourceStore_load

  // #region METHOD_ModemSourceStore_save
  // PURPOSE: Keeps rebooted modem policy valid and recoverable.
  bool save(const RuntimeModemSourceConfig& config) const;
  // #endregion METHOD_ModemSourceStore_save

 private:
  bool migrateV2Record(size_t readLength) const;
};
// #endregion CLASS_ModemSourceStore
#endif  // PERSISTENCE_CONFIG_STORE_MODEM_H
