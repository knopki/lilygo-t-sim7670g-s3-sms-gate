// #region MODULE_CONTRACT
// PURPOSE: Preserves SMTP settings so recovery can clear them independently.
// SCOPE:
// - Reads, validates, and writes the SMTP profile in appcfg.
// - NOT: SMTP delivery, socket transport, and HTTP rendering.
// INVARIANTS:
// - Credentials are stored and retrieved only as whole checksummed records.
// - Schema changes must migrate stored data.
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_smtp.h"
#include "persistence/config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kSmtpNamespace[] = "smtp";
constexpr uint16_t kDefaultSmtpPort = 587;
}  // namespace

// #region METHOD_SmtpConfigStore_load
// PURPOSE: Keeps corrupt or partial SMTP policy out of the delivery path.
bool SmtpConfigStore::load(RuntimeSmtpConfig& config) const {
  Serial.printf("event=smtp_load_begin partition=%s\n", kAppCfgPartition);
  Preferences preferences;
  if (!preferences.begin(kSmtpNamespace, true, kAppCfgPartition)) {
    Serial.println("event=smtp_load_failed reason=partition_unavailable");
    return false;
  }

  SmtpConfigRecord record{};
  const size_t readLength = preferences.getBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record) || !isSmtpConfigRecordValid(record)) {
    Serial.printf("event=smtp_load_empty_or_invalid bytes=%u\n", static_cast<unsigned>(readLength));
    return false;
  }

  config.host = record.host;
  config.port = record.port == 0 ? kDefaultSmtpPort : record.port;
  config.securityMode = record.securityMode == static_cast<uint8_t>(SmtpSecurityMode::kImplicitTls)
                            ? SmtpSecurityMode::kImplicitTls
                            : SmtpSecurityMode::kStartTls;
  config.username = record.username;
  config.password = record.password;
  config.fromAddress = record.fromAddress;
  config.recipientAddress = record.recipientAddress;
  Serial.println("event=smtp_load_complete valid=true");
  return true;
}
// #endregion METHOD_SmtpConfigStore_load

// #region FUNC_buildSmtpConfigRecord
// PURPOSE: Gives persistence one bounded SMTP representation shared with delivery tests.
SmtpConfigRecord buildSmtpConfigRecord(const RuntimeSmtpConfig& config) {
  SmtpConfigRecord record{};
  record.magic = kSmtpConfigMagic;
  record.version = kSmtpConfigVersion;
  record.port = config.port;
  record.securityMode = static_cast<uint8_t>(config.securityMode);
  config.host.toCharArray(record.host, sizeof(record.host));
  config.username.toCharArray(record.username, sizeof(record.username));
  config.password.toCharArray(record.password, sizeof(record.password));
  config.fromAddress.toCharArray(record.fromAddress, sizeof(record.fromAddress));
  config.recipientAddress.toCharArray(record.recipientAddress, sizeof(record.recipientAddress));
  record.checksum = calculateSmtpConfigChecksum(record);
  return record;
}
// #endregion FUNC_buildSmtpConfigRecord

// #region METHOD_SmtpConfigStore_save
// PURPOSE: Commits only shared-valid SMTP policy so forms and NVS cannot diverge.
bool SmtpConfigStore::save(const RuntimeSmtpConfig& config) const {
  Serial.println("event=smtp_save_begin");
  const SmtpConfigRecord record = buildSmtpConfigRecord(config);
  if (!isSmtpConfigRecordValid(record)) {
    Serial.println("event=smtp_save_failed reason=invalid_fields");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kSmtpNamespace, false, kAppCfgPartition)) {
    Serial.println("event=smtp_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=smtp_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion METHOD_SmtpConfigStore_save
