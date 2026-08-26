// #region MODULE_CONTRACT
// PURPOSE: Persists SMTP delivery profile in the isolated appcfg NVS
// partition so USB recovery can erase it without touching other settings.
// SCOPE: load/save of SmtpConfigStore profile and buildSmtpConfigRecord
// helper.
// NOT: Wi-Fi, ZTE, and modem-source profiles, network connection attempts,
// and protocol dialogs.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_SMTP_H
#define PERSISTENCE_CONFIG_STORE_SMTP_H

#include <Arduino.h>

#include "smtp/smtp_record.h"

struct RuntimeSmtpConfig {
  String host;
  uint16_t port = 587;
  SmtpSecurityMode securityMode = SmtpSecurityMode::kStartTls;
  String username;
  String password;
  String fromAddress;
  String recipientAddress;
};

SmtpConfigRecord buildSmtpConfigRecord(const RuntimeSmtpConfig& config);

class SmtpConfigStore {
 public:
  bool load(RuntimeSmtpConfig& config) const;
  bool save(const RuntimeSmtpConfig& config) const;
};
#endif  // PERSISTENCE_CONFIG_STORE_SMTP_H
