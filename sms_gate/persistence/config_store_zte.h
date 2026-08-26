// #region MODULE_CONTRACT
// PURPOSE: Persists ZTE MF79RU source profile with V1/V2->V3 migration
// SCOPE: load/save of ZteConfigStore profile and buildZteConfigRecord helper with V1/V2->V3
// migration. NOT: Wi-Fi, SMTP, and modem-source profiles, network connection attempts, and protocol
// dialogs. INVARIANTS: Credentials are stored as a whole checksummed record. DEPENDENCIES:
// Preferences partition appcfg. #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_ZTE_H
#define PERSISTENCE_CONFIG_STORE_ZTE_H

#include <Arduino.h>

#include "zte/zte_record.h"

struct RuntimeZteConfig {
  bool enabled = false;
  String host;
  String password;
  String label;  // Phone number or alias shown in forwarded emails.
  uint16_t pollIntervalSec = kDefaultZtePollSec;
};

ZteConfigRecord buildZteConfigRecord(const RuntimeZteConfig& config);

class ZteConfigStore {
 public:
  bool load(RuntimeZteConfig& config) const;
  bool save(const RuntimeZteConfig& config) const;

 private:
  bool migrateV1Record(size_t readLength) const;
  bool migrateV2Record(size_t readLength) const;
};
#endif  // PERSISTENCE_CONFIG_STORE_ZTE_H
