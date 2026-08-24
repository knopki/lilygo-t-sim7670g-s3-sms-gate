// #region MODULE_CONTRACT
// PURPOSE: Provides validated access to the isolated appcfg NVS partition for
// the Wi-Fi, web administrator, SMTP delivery, and ZTE SMS source
// configurations.
// SCOPE:
// - Password validation, constant-time comparison, and record load/save for
// the network, SMTP, and ZTE profiles.
// - NOT: Network connection attempts, protocol dialogs, and HTTP request
// handling.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

#include "config_record.h"
#include "smtp_record.h"
#include "zte_record.h"

struct RuntimeConfig {
  String ssid;
  String wifiPassword;
  String adminPassword;
};

struct RuntimeSmtpConfig {
  String host;
  uint16_t port = 587;
  SmtpSecurityMode securityMode = SmtpSecurityMode::kStartTls;
  String username;
  String password;
  String fromAddress;
  String recipientAddress;
};

struct RuntimeZteConfig {
  bool enabled = false;
  String host;
  String password;
  String label;  // Phone number or alias shown in forwarded emails.
};

bool isPrintableAscii(const String& value);
bool isValidPassword(const String& value);
bool constantTimeEquals(const String& left, const String& right);
SmtpConfigRecord buildSmtpConfigRecord(const RuntimeSmtpConfig& config);

class ConfigStore {
 public:
  bool load(RuntimeConfig& config) const;
  bool save(const RuntimeConfig& config) const;
};

class SmtpConfigStore {
 public:
  bool load(RuntimeSmtpConfig& config) const;
  bool save(const RuntimeSmtpConfig& config) const;
};

class ZteConfigStore {
 public:
  bool load(RuntimeZteConfig& config) const;
  bool save(const RuntimeZteConfig& config) const;

 private:
  // Rewrites a stored pre-label v1 record as v2; see config_store.cpp.
  bool migrateV1Record(size_t readLength) const;
};
