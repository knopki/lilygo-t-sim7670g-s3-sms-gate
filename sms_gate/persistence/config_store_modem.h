// #region MODULE_CONTRACT
// PURPOSE: Persists onboard SIM7670G source profile in the isolated appcfg
// NVS partition so USB recovery can erase it without touching other settings.
// SCOPE: load/save of ModemSourceStore profile and buildModemSourceRecord helper.
// NOT: Wi-Fi, SMTP, and ZTE profiles, network connection attempts, and protocol dialogs.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_MODEM_H
#define PERSISTENCE_CONFIG_STORE_MODEM_H

#include <Arduino.h>

#include "modem/modem_record.h"

struct RuntimeModemSourceConfig {
  bool enabled = false;
  uint16_t pollIntervalSec = kDefaultModemPollSec;
  String label;  // Phone number or alias shown in forwarded emails.
};

ModemSourceRecord buildModemSourceRecord(const RuntimeModemSourceConfig& config);

class ModemSourceStore {
 public:
  bool load(RuntimeModemSourceConfig& config) const;
  bool save(const RuntimeModemSourceConfig& config) const;
};
#endif  // PERSISTENCE_CONFIG_STORE_MODEM_H
