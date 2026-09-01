// #region MODULE_CONTRACT
// PURPOSE: Preserves SMTP settings so recovery can clear them independently.
// SCOPE:
// - load/save of SmtpConfigStore profile and buildSmtpConfigRecord helper.
// - NOT: Wi-Fi, ZTE, and modem-source profiles, network connection attempts, and protocol dialogs.
// INVARIANTS: Credentials are stored as a whole checksummed record.
// DEPENDENCIES: Preferences partition appcfg.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_SMTP_H
#define PERSISTENCE_CONFIG_STORE_SMTP_H

#include <Arduino.h>

#include "smtp/smtp_record.h"

// #region STRUCT_RuntimeSmtpConfig
// PURPOSE: Carries validated SMTP settings between web, runtime, and NVS boundaries.
struct RuntimeSmtpConfig {
  String host;
  uint16_t port = 587;
  SmtpSecurityMode securityMode = SmtpSecurityMode::kStartTls;
  String username;
  String password;
  String fromAddress;
  String recipientAddress;
};
// #endregion STRUCT_RuntimeSmtpConfig

// #region FUNC_buildSmtpConfigRecord
// PURPOSE: Gives persistence one bounded representation of SMTP settings.
SmtpConfigRecord buildSmtpConfigRecord(const RuntimeSmtpConfig& config);
// #endregion FUNC_buildSmtpConfigRecord

// #region CLASS_SmtpConfigStore
// PURPOSE: Owns atomic persistence of SMTP settings in the recovery-safe partition.
class SmtpConfigStore {
 public:
  // #region METHOD_SmtpConfigStore_load
  // PURPOSE: Keeps corrupt or partial SMTP policy out of delivery.
  bool load(RuntimeSmtpConfig& config) const;
  // #endregion METHOD_SmtpConfigStore_load

  // #region METHOD_SmtpConfigStore_save
  // PURPOSE: Keeps rebooted SMTP policy valid and recoverable.
  bool save(const RuntimeSmtpConfig& config) const;
  // #endregion METHOD_SmtpConfigStore_save
};
// #endregion CLASS_SmtpConfigStore
#endif  // PERSISTENCE_CONFIG_STORE_SMTP_H
