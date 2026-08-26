// #region MODULE_CONTRACT
// PURPOSE: Implements network NVS store for the verified Wi-Fi and
// administrator profile in the isolated appcfg partition.
// #endregion MODULE_CONTRACT

#include "config_store_network.h"

#include <Preferences.h>

namespace {
constexpr char kConfigNamespace[] = "network";
}  // namespace

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
  record.checksum = calculateConfigChecksum(record);

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
