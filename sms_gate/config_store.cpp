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
constexpr char kZteNamespace[] = "zte";
constexpr char kModemNamespace[] = "modem";
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

// #region FUNC_buildZteConfigRecord
// PURPOSE: Converts the runtime ZTE profile into its checksummed binary
// record so persistence and the web form always exercise the same field
// limits.
ZteConfigRecord buildZteConfigRecord(const RuntimeZteConfig& config) {
  ZteConfigRecord record{};
  record.magic = kZteConfigMagic;
  record.version = kZteConfigVersion;
  record.enabled = config.enabled ? 1 : 0;
  config.host.toCharArray(record.host, sizeof(record.host));
  config.password.toCharArray(record.password, sizeof(record.password));
  config.label.toCharArray(record.label, sizeof(record.label));
  record.pollIntervalSec = config.pollIntervalSec;
  if (!isValidZtePollInterval(record.pollIntervalSec)) {
    record.pollIntervalSec = kDefaultZtePollSec;
  }
  record.checksum = calculateZteConfigChecksum(record);
  return record;
}
// #endregion FUNC_buildZteConfigRecord

// #region FUNC_migrateV1Record
// PURPOSE: Recognizes a stored pre-label v1 ZTE record and rewrites it as a
// valid v3 record with an empty label and default poll interval.
bool ZteConfigStore::migrateV1Record(size_t readLength) const {
  if (readLength != sizeof(ZteConfigRecordV1)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kZteNamespace, true, kConfigPartition)) {
    return false;
  }
  ZteConfigRecordV1 legacy{};
  const size_t legacyLength = preferences.getBytes(kConfigKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isZteConfigRecordV1Valid(legacy)) {
    return false;
  }

  RuntimeZteConfig migrated;
  migrated.enabled = legacy.enabled == 1;
  migrated.host = legacy.host;
  migrated.password = legacy.password;
  migrated.label = "";
  migrated.pollIntervalSec = kDefaultZtePollSec;
  const ZteConfigRecord record = buildZteConfigRecord(migrated);
  if (!isZteConfigRecordValid(record) ||
      !preferences.begin(kZteNamespace, false, kConfigPartition)) {
    Serial.println("event=zte_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=zte_load_migrated from_version=1 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion FUNC_migrateV1Record

// #region FUNC_migrateV2Record
// PURPOSE: Recognizes a stored v2 ZTE record (without poll interval) and
// rewrites it as v3 with default 15 s interval.
bool ZteConfigStore::migrateV2Record(size_t readLength) const {
  if (readLength != sizeof(ZteConfigRecordV2)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kZteNamespace, true, kConfigPartition)) {
    return false;
  }
  ZteConfigRecordV2 legacy{};
  const size_t legacyLength = preferences.getBytes(kConfigKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isZteConfigRecordV2Valid(legacy)) {
    return false;
  }

  RuntimeZteConfig migrated;
  migrated.enabled = legacy.enabled == 1;
  migrated.host = legacy.host;
  migrated.password = legacy.password;
  migrated.label = legacy.label;
  migrated.pollIntervalSec = kDefaultZtePollSec;
  const ZteConfigRecord record = buildZteConfigRecord(migrated);
  if (!isZteConfigRecordValid(record) ||
      !preferences.begin(kZteNamespace, false, kConfigPartition)) {
    Serial.println("event=zte_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=zte_load_migrated from_version=2 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion FUNC_migrateV2Record

// #region FUNC_ZteConfigStore_load
// PURPOSE: Restores the ZTE profile only as a whole validated record so a
// corrupt or partial blob can never reach the modem dialog; stored v1/v2
// records are migrated to v3 in place.
bool ZteConfigStore::load(RuntimeZteConfig& config) const {
  Serial.printf("event=zte_load_begin partition=%s\n", kConfigPartition);
  Preferences preferences;
  if (!preferences.begin(kZteNamespace, true, kConfigPartition)) {
    Serial.println("event=zte_load_failed reason=partition_unavailable");
    return false;
  }

  ZteConfigRecord record{};
  const size_t readLength = preferences.getBytes(kConfigKey, &record, sizeof(record));
  preferences.end();

  if (readLength != sizeof(record)) {
    if (migrateV1Record(readLength) || migrateV2Record(readLength)) {
      return load(config);  // Re-read as the freshly written v3 record.
    }
    Serial.printf("event=zte_load_empty_or_invalid bytes=%u\n", static_cast<unsigned>(readLength));
    return false;
  }
  if (!isZteConfigRecordValid(record)) {
    Serial.printf("event=zte_load_empty_or_invalid bytes=%u\n", static_cast<unsigned>(readLength));
    return false;
  }

  config.enabled = record.enabled == 1;
  config.host = record.host;
  config.password = record.password;
  config.label = record.label;
  config.pollIntervalSec = record.pollIntervalSec;
  Serial.printf("event=zte_load_complete enabled=%s poll_interval=%u\n",
                config.enabled ? "true" : "false", static_cast<unsigned>(config.pollIntervalSec));
  return true;
}
// #endregion FUNC_ZteConfigStore_load

// #region FUNC_ZteConfigStore_save
// PURPOSE: Persists the ZTE profile only after the full record passes the
// shared validation predicate, so web input and NVS content agree.
bool ZteConfigStore::save(const RuntimeZteConfig& config) const {
  Serial.println("event=zte_save_begin");
  const ZteConfigRecord record = buildZteConfigRecord(config);
  if (!isZteConfigRecordValid(record)) {
    Serial.println("event=zte_save_failed reason=invalid_fields");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kZteNamespace, false, kConfigPartition)) {
    Serial.println("event=zte_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=zte_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_ZteConfigStore_save

// #region FUNC_buildModemSourceRecord
// PURPOSE: Converts the runtime modem-source profile into its checksummed
// binary record so persistence and the web form always exercise the same
// field limits.
ModemSourceRecord buildModemSourceRecord(const RuntimeModemSourceConfig& config) {
  ModemSourceRecord record{};
  record.magic = kModemSourceMagic;
  record.version = kModemSourceVersion;
  record.enabled = config.enabled ? 1 : 0;
  record.pollIntervalSec = config.pollIntervalSec;
  config.label.toCharArray(record.label, sizeof(record.label));
  record.checksum = calculateModemSourceChecksum(record);
  return record;
}
// #endregion FUNC_buildModemSourceRecord

// #region FUNC_ModemSourceStore_load
// PURPOSE: Restores the modem-source profile only as a whole validated
// record so a corrupt or partial blob can never reach the SMS poll path.
bool ModemSourceStore::load(RuntimeModemSourceConfig& config) const {
  Serial.printf("event=modem_source_load_begin partition=%s\n", kConfigPartition);
  Preferences preferences;
  if (!preferences.begin(kModemNamespace, true, kConfigPartition)) {
    Serial.println("event=modem_source_load_failed reason=partition_unavailable");
    return false;
  }

  ModemSourceRecord record{};
  const size_t readLength = preferences.getBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record) || !isModemSourceRecordValid(record)) {
    Serial.printf("event=modem_source_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }

  config.enabled = record.enabled == 1;
  config.pollIntervalSec = record.pollIntervalSec;
  config.label = record.label;
  Serial.printf("event=modem_source_load_complete enabled=%s poll_interval=%u\n",
                config.enabled ? "true" : "false", static_cast<unsigned>(config.pollIntervalSec));
  return true;
}
// #endregion FUNC_ModemSourceStore_load

// #region FUNC_ModemSourceStore_save
// PURPOSE: Persists the modem-source profile only after the full record
// passes the shared validation predicate, so web input and NVS content agree.
bool ModemSourceStore::save(const RuntimeModemSourceConfig& config) const {
  Serial.println("event=modem_source_save_begin");
  const ModemSourceRecord record = buildModemSourceRecord(config);
  if (!isModemSourceRecordValid(record)) {
    Serial.println("event=modem_source_save_failed reason=invalid_fields");
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kModemNamespace, false, kConfigPartition)) {
    Serial.println("event=modem_source_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kConfigKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=modem_source_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_ModemSourceStore_save
