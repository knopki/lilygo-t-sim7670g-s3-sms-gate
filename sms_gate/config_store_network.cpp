// #region MODULE_CONTRACT
// PURPOSE: Implements network NVS store for the verified Wi-Fi and
// administrator profile in the isolated appcfg partition.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_network.h"

#include <Preferences.h>

namespace {
constexpr char kConfigNamespace[] = "network";
}  // namespace

// #region METHOD_ConfigStore_migrateV1Record
bool ConfigStore::migrateV1Record(size_t readLength) const {
  if (readLength != sizeof(ConfigRecordV1)) return false;
  Preferences preferences;
  if (!preferences.begin(kConfigNamespace, true, kAppCfgPartition)) return false;
  ConfigRecordV1 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isConfigRecordV1Valid(legacy)) return false;

  ConfigRecord record{};
  record.magic = kConfigMagic;
  record.version = kConfigVersion;
  memcpy(record.ssid, legacy.ssid, sizeof(record.ssid));
  memcpy(record.wifiPassword, legacy.wifiPassword, sizeof(record.wifiPassword));
  memcpy(record.adminPassword, legacy.adminPassword, sizeof(record.adminPassword));
  strncpy(record.ntpServer1, kDefaultNtpServer1, sizeof(record.ntpServer1) - 1);
  strncpy(record.ntpServer2, kDefaultNtpServer2, sizeof(record.ntpServer2) - 1);
  record.ntpEnabled = 1;
  record.checksum = calculateConfigChecksum(record);
  if (!isConfigRecordValid(record)) return false;
  if (!preferences.begin(kConfigNamespace, false, kAppCfgPartition)) {
    Serial.println("event=config_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=config_load_migrated from_version=1 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion METHOD_ConfigStore_migrateV1Record

// #region METHOD_ConfigStore_load
// PURPOSE: Restores the verified Wi-Fi and administrator profile as a
// whole checksummed record.
bool ConfigStore::load(RuntimeConfig& config) const {
  Serial.printf("event=config_load_begin partition=%s\n", kAppCfgPartition);
  Preferences preferences;
  if (!preferences.begin(kConfigNamespace, true, kAppCfgPartition)) {
    Serial.println("event=config_load_failed reason=partition_unavailable");
    return false;
  }

  ConfigRecord record{};
  const size_t readLength = preferences.getBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record)) {
    if (migrateV1Record(readLength)) return load(config);
    Serial.printf("event=config_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }
  if (!isConfigRecordValid(record)) {
    Serial.printf("event=config_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }

  record.ssid[kMaxSsidLength] = '\0';
  record.wifiPassword[kMaxPasswordLength] = '\0';
  record.adminPassword[kMaxPasswordLength] = '\0';
  record.ntpServer1[kMaxNtpServerLength] = '\0';
  record.ntpServer2[kMaxNtpServerLength] = '\0';
  config.ssid = record.ssid;
  config.wifiPassword = record.wifiPassword;
  config.adminPassword = record.adminPassword;
  config.ntpServer1 = record.ntpServer1;
  config.ntpServer2 = record.ntpServer2;
  config.ntpEnabled = record.ntpEnabled == 1;
  // Backfill defaults for empty servers when enabled (fresh v2 with empty NVS).
  if (config.ntpServer1.length() == 0 && config.ntpServer2.length() == 0 && config.ntpEnabled) {
    config.ntpServer1 = kDefaultNtpServer1;
    config.ntpServer2 = kDefaultNtpServer2;
  }
  const bool valid = config.ssid.length() > 0 && isValidPassword(config.wifiPassword) &&
                     isValidPassword(config.adminPassword);
  Serial.printf("event=config_load_complete valid=%s ntp_enabled=%s\n", valid ? "true" : "false",
                config.ntpEnabled ? "true" : "false");
  return valid;
}
// #endregion METHOD_ConfigStore_load

// #region METHOD_ConfigStore_save
// PURPOSE: Persists the verified Wi-Fi and administrator profile as a
// whole checksummed record.
bool ConfigStore::save(const RuntimeConfig& config) const {
  Serial.println("event=config_save_begin");
  ConfigRecord record{};
  record.magic = kConfigMagic;
  record.version = kConfigVersion;
  config.ssid.toCharArray(record.ssid, sizeof(record.ssid));
  config.wifiPassword.toCharArray(record.wifiPassword, sizeof(record.wifiPassword));
  config.adminPassword.toCharArray(record.adminPassword, sizeof(record.adminPassword));
  // NTP servers: store printable, truncated to field size; empty allowed.
  config.ntpServer1.toCharArray(record.ntpServer1, sizeof(record.ntpServer1));
  config.ntpServer2.toCharArray(record.ntpServer2, sizeof(record.ntpServer2));
  record.ntpEnabled = config.ntpEnabled ? 1 : 0;
  // Ensure NUL termination (toCharArray does it, but keep explicit).
  record.ssid[kMaxSsidLength] = '\0';
  record.wifiPassword[kMaxPasswordLength] = '\0';
  record.adminPassword[kMaxPasswordLength] = '\0';
  record.ntpServer1[kMaxNtpServerLength] = '\0';
  record.ntpServer2[kMaxNtpServerLength] = '\0';
  record.checksum = calculateConfigChecksum(record);
  if (!isConfigRecordValid(record)) {
    Serial.println("event=config_save_failed reason=invalid_fields");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kConfigNamespace, false, kAppCfgPartition)) {
    Serial.println("event=config_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=config_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion METHOD_ConfigStore_save
