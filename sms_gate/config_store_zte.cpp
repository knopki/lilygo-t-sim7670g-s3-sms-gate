// #region MODULE_CONTRACT
// PURPOSE: Persists verified records in a dedicated NVS partition so USB
// recovery can erase them without touching future independent settings.
// INVARIANTS: Credentials are stored and retrieved only as whole checksummed
// records. Schema changes must migrate stored data (see AGENTS.md).
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "config_store_zte.h"
#include "config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kZteNamespace[] = "zte";
}  // namespace

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
  if (!preferences.begin(kZteNamespace, true, kAppCfgPartition)) {
    return false;
  }
  ZteConfigRecordV1 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
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
      !preferences.begin(kZteNamespace, false, kAppCfgPartition)) {
    Serial.println("event=zte_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
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
  if (!preferences.begin(kZteNamespace, true, kAppCfgPartition)) {
    return false;
  }
  ZteConfigRecordV2 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
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
      !preferences.begin(kZteNamespace, false, kAppCfgPartition)) {
    Serial.println("event=zte_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
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
  Serial.printf("event=zte_load_begin partition=%s\n", kAppCfgPartition);
  Preferences preferences;
  if (!preferences.begin(kZteNamespace, true, kAppCfgPartition)) {
    Serial.println("event=zte_load_failed reason=partition_unavailable");
    return false;
  }

  ZteConfigRecord record{};
  const size_t readLength = preferences.getBytes(kAppCfgKey, &record, sizeof(record));
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
  if (!preferences.begin(kZteNamespace, false, kAppCfgPartition)) {
    Serial.println("event=zte_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=zte_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_ZteConfigStore_save
