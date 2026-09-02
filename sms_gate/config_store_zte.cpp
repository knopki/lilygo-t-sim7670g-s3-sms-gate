// #region MODULE_CONTRACT
// PURPOSE: Preserves ZTE settings so recovery can clear them independently.
// SCOPE:
// - Reads, validates, migrates, and writes the ZTE profile in appcfg.
// - NOT: ZTE modem dialogs, SMS forwarding, and HTTP rendering.
// INVARIANTS:
// - Credentials are stored and retrieved only as whole checksummed records.
// - Schema changes must migrate stored data.
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_zte.h"
#include "persistence/config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kZteNamespace[] = "zte";
}  // namespace

// #region FUNC_buildZteConfigRecord
// PURPOSE: Gives persistence one bounded ZTE representation shared with forms.
ZteConfigRecord buildZteConfigRecord(const RuntimeZteConfig& config) {
  ZteConfigRecord record{};
  record.magic = kZteConfigMagic;
  record.version = kZteConfigVersion;
  record.moduleEnabled = config.moduleEnabled ? 1 : 0;
  record.forwardEnabled = config.forwardEnabled ? 1 : 0;
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

// #region METHOD_ZteConfigStore_migrateV3Record
// PURPOSE: Upgrades a valid legacy ZTE blob without losing its settings.
bool ZteConfigStore::migrateV3Record(size_t readLength) const {
  if (readLength != sizeof(ZteConfigRecordV3)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kZteNamespace, true, kAppCfgPartition)) {
    return false;
  }
  ZteConfigRecordV3 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isZteConfigRecordV3Valid(legacy)) {
    return false;
  }

  RuntimeZteConfig migrated;
  migrated.moduleEnabled = legacy.enabled == 1;
  migrated.forwardEnabled = legacy.enabled == 1;
  migrated.host = legacy.host;
  migrated.password = legacy.password;
  migrated.label = legacy.label;
  migrated.pollIntervalSec = legacy.pollIntervalSec;
  const ZteConfigRecord record = buildZteConfigRecord(migrated);
  if (!isZteConfigRecordValid(record) ||
      !preferences.begin(kZteNamespace, false, kAppCfgPartition)) {
    Serial.println("event=zte_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=zte_load_migrated from_version=3 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion METHOD_ZteConfigStore_migrateV3Record

// #region METHOD_ZteConfigStore_load
// PURPOSE: Keeps corrupt or partial ZTE policy out of the modem dialog.
// Stored v3 records are migrated to v4 in place (sample kept, older removed).
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

  if (readLength != sizeof(record) || !isZteConfigRecordValid(record)) {
    if (migrateV3Record(readLength)) {
      return load(config);
    }
    Serial.printf("event=zte_load_empty_or_invalid bytes=%u\n", static_cast<unsigned>(readLength));
    return false;
  }

  config.moduleEnabled = record.moduleEnabled == 1;
  config.forwardEnabled = record.forwardEnabled == 1;
  config.host = record.host;
  config.password = record.password;
  config.label = record.label;
  config.pollIntervalSec = record.pollIntervalSec;
  Serial.printf("event=zte_load_complete module=%s forward=%s poll_interval=%u\n",
                config.moduleEnabled ? "true" : "false", config.forwardEnabled ? "true" : "false",
                static_cast<unsigned>(config.pollIntervalSec));
  return true;
}
// #endregion METHOD_ZteConfigStore_load

// #region METHOD_ZteConfigStore_save
// PURPOSE: Commits only shared-valid ZTE policy so forms and NVS cannot diverge.
bool ZteConfigStore::save(const RuntimeZteConfig& config) const {
  Serial.println("event=zte_save_begin");
  if (config.host.length() >= sizeof(ZteConfigRecord::host) ||
      config.password.length() >= sizeof(ZteConfigRecord::password) ||
      config.label.length() >= sizeof(ZteConfigRecord::label)) {
    Serial.println("event=zte_save_failed reason=field_too_long");
    return false;
  }
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
// #endregion METHOD_ZteConfigStore_save
