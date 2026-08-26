// #region MODULE_CONTRACT
// PURPOSE: Persists the verified Wi-Fi and administrator profile in isolated
// appcfg NVS so USB recovery can erase it without touching other settings.
// SCOPE: load/save of ConfigStore network profile.
// NOT: SMTP/ZTE/modem delivery or protocol handling.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

#include "config_record.h"
#include "config_store_common.h"

struct RuntimeConfig {
  String ssid;
  String wifiPassword;
  String adminPassword;
};

class ConfigStore {
 public:
  bool load(RuntimeConfig& config) const;
  bool save(const RuntimeConfig& config) const;
};
