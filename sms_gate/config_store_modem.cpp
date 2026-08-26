// #region MODULE_CONTRACT
// PURPOSE: Persists verified records in a dedicated NVS partition so USB
// recovery can erase them without touching future independent settings.
// INVARIANTS: Credentials are stored and retrieved only as whole checksummed
// records. Schema changes must migrate stored data (see AGENTS.md).
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_modem.h"
#include "persistence/config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kModemNamespace[] = "modem";
}  // namespace

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
  record.nitzTimeSyncEnabled = config.nitzTimeSyncEnabled ? 1 : 0;
  record.checksum = calculateModemSourceChecksum(record);
  return record;
}
// #endregion FUNC_buildModemSourceRecord

// #region FUNC_ModemSourceStore_migrateV1Record
bool ModemSourceStore::migrateV1Record(size_t readLength) const {
  if (readLength != sizeof(ModemSourceRecordV1)) return false;
  Preferences preferences;
  if (!preferences.begin(kModemNamespace, true, kAppCfgPartition)) return false;
  ModemSourceRecordV1 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isModemSourceRecordV1Valid(legacy)) return false;

  RuntimeModemSourceConfig migrated;
  migrated.enabled = legacy.enabled == 1;
  migrated.pollIntervalSec = legacy.pollIntervalSec;
  migrated.label = legacy.label;
  migrated.nitzTimeSyncEnabled = true;
  const ModemSourceRecord record = buildModemSourceRecord(migrated);
  if (!isModemSourceRecordValid(record)) return false;
  if (!preferences.begin(kModemNamespace, false, kAppCfgPartition)) {
    Serial.println("event=modem_source_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=modem_source_load_migrated from_version=1 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion FUNC_ModemSourceStore_migrateV1Record

// #region FUNC_ModemSourceStore_load
// PURPOSE: Restores the modem-source profile only as a whole validated
// record so a corrupt or partial blob can never reach the SMS poll path.
bool ModemSourceStore::load(RuntimeModemSourceConfig& config) const {
  Serial.printf("event=modem_source_load_begin partition=%s\n", kAppCfgPartition);
  Preferences preferences;
  if (!preferences.begin(kModemNamespace, true, kAppCfgPartition)) {
    Serial.println("event=modem_source_load_failed reason=partition_unavailable");
    return false;
  }

  ModemSourceRecord record{};
  const size_t readLength = preferences.getBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record)) {
    if (migrateV1Record(readLength)) return load(config);
    Serial.printf("event=modem_source_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }
  if (!isModemSourceRecordValid(record)) {
    Serial.printf("event=modem_source_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }

  config.enabled = record.enabled == 1;
  config.pollIntervalSec = record.pollIntervalSec;
  config.label = record.label;
  config.nitzTimeSyncEnabled = record.nitzTimeSyncEnabled == 1;
  Serial.printf("event=modem_source_load_complete enabled=%s poll_interval=%u nitz_time_sync=%s\n",
                config.enabled ? "true" : "false", static_cast<unsigned>(config.pollIntervalSec),
                config.nitzTimeSyncEnabled ? "true" : "false");
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
  if (!preferences.begin(kModemNamespace, false, kAppCfgPartition)) {
    Serial.println("event=modem_source_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=modem_source_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_ModemSourceStore_save
