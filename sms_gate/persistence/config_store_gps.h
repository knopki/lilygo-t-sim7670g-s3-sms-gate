// #region MODULE_CONTRACT
// PURPOSE: Persists GNSS polling profile in the isolated appcfg NVS partition
// so USB recovery can erase it without touching other settings.
// SCOPE: load/save of GpsConfigStore profile and buildGpsRecord helper.
// NOT: Wi-Fi, SMTP, ZTE and modem-source profiles, network connections and
// GNSS AT dialogs.
// INVARIANTS: Profile stored as whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_GPS_H
#define PERSISTENCE_CONFIG_STORE_GPS_H

#include <Arduino.h>

#include "gps/gps_record.h"

struct RuntimeGpsConfig {
  bool enabled = false;
  uint16_t pollIntervalSec = kDefaultGpsPollSec;
};

GpsRecord buildGpsRecord(const RuntimeGpsConfig& config);

class GpsConfigStore {
 public:
  bool load(RuntimeGpsConfig& config) const;
  bool save(const RuntimeGpsConfig& config) const;
};

#endif  // PERSISTENCE_CONFIG_STORE_GPS_H
