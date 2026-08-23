// #region MODULE_CONTRACT
// PURPOSE: Provides validated access to the isolated appcfg NVS partition for
// the Wi-Fi and web administrator configuration.
// SCOPE:
// - Password validation, constant-time comparison, and one-record
// load/save.
// - NOT: Network connection attempts and HTTP request handling.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

#include "config_record.h"

struct RuntimeConfig {
  String ssid;
  String wifiPassword;
  String adminPassword;
};

bool isPrintableAscii(const String& value);
bool isValidPassword(const String& value);
bool constantTimeEquals(const String& left, const String& right);

class ConfigStore {
 public:
  bool load(RuntimeConfig& config) const;
  bool save(const RuntimeConfig& config) const;
};
