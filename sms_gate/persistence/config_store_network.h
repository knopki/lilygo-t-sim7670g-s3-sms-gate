// #region MODULE_CONTRACT
// PURPOSE: Preserves network access so recovery can clear it independently.
// SCOPE:
// - load/save of ConfigStore network profile.
// - NOT: SMTP/ZTE/modem delivery or protocol handling.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_NETWORK_H
#define PERSISTENCE_CONFIG_STORE_NETWORK_H

#include <Arduino.h>

#include "persistence/config_record.h"
#include "persistence/config_store_common.h"

constexpr char kDefaultNtpServer1[] = "pool.ntp.org";
constexpr char kDefaultNtpServer2[] = "time.nist.gov";

// #region STRUCT_RuntimeConfig
// PURPOSE: Carries the validated network profile between web, runtime, and NVS boundaries.
struct RuntimeConfig {
  String ssid;
  String wifiPassword;
  String adminPassword;
  String ntpServer1 = kDefaultNtpServer1;
  String ntpServer2 = kDefaultNtpServer2;
  bool ntpEnabled = true;
};
// #endregion STRUCT_RuntimeConfig

// #region CLASS_ConfigStore
// PURPOSE: Owns atomic persistence of the network profile in the recovery-safe partition.
class ConfigStore {
 public:
  // #region METHOD_ConfigStore_load
  // PURPOSE: Restores only a valid network profile from appcfg.
  bool load(RuntimeConfig& config) const;
  // #endregion METHOD_ConfigStore_load

  // #region METHOD_ConfigStore_save
  // PURPOSE: Commits a validated network profile atomically.
  bool save(const RuntimeConfig& config) const;
  // #endregion METHOD_ConfigStore_save

 private:
  bool migrateV1Record(size_t readLength) const;
};
// #endregion CLASS_ConfigStore
#endif  // PERSISTENCE_CONFIG_STORE_NETWORK_H
