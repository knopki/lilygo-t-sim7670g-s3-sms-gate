// #region MODULE_CONTRACT
// PURPOSE: Persists verified records in a dedicated NVS partition so USB
// recovery can erase them without touching future independent settings.
// INVARIANTS: Credentials are stored and retrieved only as whole checksummed
// records. Schema changes must migrate stored data (see AGENTS.md).
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "config_store.h"

#include <Preferences.h>

namespace {
constexpr char kConfigPartition[] = "appcfg";
constexpr char kConfigNamespace[] = "network";
constexpr char kConfigKey[] = "record";
constexpr char kSmtpNamespace[] = "smtp";
constexpr uint16_t kDefaultSmtpPort = 587;
}  // namespace

bool isPrintableAscii(const String& value) {
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < 32 || character > 126) {
      return false;
    }
  }
  return true;
}

bool isValidPassword(const String& value) {
  return value.length() >= kMinPasswordLength && value.length() <= kMaxPasswordLength &&
         isPrintableAscii(value);
}

bool constantTimeEquals(const String& left, const String& right) {
  const size_t longestLength = max(left.length(), right.length());
  uint8_t difference = static_cast<uint8_t>(left.length() ^ right.length());
  for (size_t index = 0; index < longestLength; ++index) {
    const char leftCharacter = index < left.length() ? left[index] : 0;
    const char rightCharacter = index < right.length() ? right[index] : 0;
    difference |= static_cast<uint8_t>(leftCharacter ^ rightCharacter);
  }
  return difference == 0;
}

bool ConfigStore::load(RuntimeConfig& config) const {
  Serial.printf("event=config_load_begin partition=%s\n", kConfigPartition);
  Preferences preferences;
  if (!preferences.begin(kConfigNamespace, true, kConfigPartition)) {
    Serial.println("event=config_load_failed reason=partition_unavailable");
    return false;
  }

  ConfigRecord record{};
  const size_t readLength = preferences.getBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record) || record.magic != kConfigMagic ||
      record.version != kConfigVersion || record.checksum != calculateConfigChecksum(record)) {
    Serial.printf("event=config_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }

  record.ssid[kMaxSsidLength] = '\0';
  record.wifiPassword[kMaxPasswordLength] = '\0';
  record.adminPassword[kMaxPasswordLength] = '\0';
  config.ssid = record.ssid;
  config.wifiPassword = record.wifiPassword;
  config.adminPassword = record.adminPassword;
  const bool valid = config.ssid.length() > 0 && isValidPassword(config.wifiPassword) &&
                     isValidPassword(config.adminPassword);
  Serial.printf("event=config_load_complete valid=%s\n", valid ? "true" : "false");
  return valid;
}

bool ConfigStore::save(const RuntimeConfig& config) const {
  Serial.println("event=config_save_begin");
  ConfigRecord record{};
  record.magic = kConfigMagic;
  record.version = kConfigVersion;
  config.ssid.toCharArray(record.ssid, sizeof(record.ssid));
  config.wifiPassword.toCharArray(record.wifiPassword, sizeof(record.wifiPassword));
  config.adminPassword.toCharArray(record.adminPassword, sizeof(record.adminPassword));
  record.checksum = calculateConfigChecksum(record);

  Preferences preferences;
  if (!preferences.begin(kConfigNamespace, false, kConfigPartition)) {
    Serial.println("event=config_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=config_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}

// #region FUNC_SmtpConfigStore_load
// PURPOSE: Restores the SMTP profile only as a whole validated record so a
// corrupt or partial blob can never reach the delivery path. Schema changes
// must migrate stored data (see AGENTS.md); nothing here upgrades records.
bool SmtpConfigStore::load(RuntimeSmtpConfig& config) const {
  Serial.printf("event=smtp_load_begin partition=%s\n", kConfigPartition);
  Preferences preferences;
  if (!preferences.begin(kSmtpNamespace, true, kConfigPartition)) {
    Serial.println("event=smtp_load_failed reason=partition_unavailable");
    return false;
  }

  SmtpConfigRecord record{};
  const size_t readLength = preferences.getBytes(kConfigKey, &record, sizeof(record));
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
// #endregion FUNC_SmtpConfigStore_load

// #region FUNC_buildSmtpConfigRecord
// PURPOSE: Converts the runtime profile into its checksummed binary record so
// persistence and test delivery always exercise the same field limits.
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

// #region FUNC_SmtpConfigStore_save
// PURPOSE: Persists the SMTP profile only after the full record passes the
// shared validation predicate, so web input and NVS content agree.
bool SmtpConfigStore::save(const RuntimeSmtpConfig& config) const {
  Serial.println("event=smtp_save_begin");
  const SmtpConfigRecord record = buildSmtpConfigRecord(config);
  if (!isSmtpConfigRecordValid(record)) {
    Serial.println("event=smtp_save_failed reason=invalid_fields");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kSmtpNamespace, false, kConfigPartition)) {
    Serial.println("event=smtp_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=smtp_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_SmtpConfigStore_save
