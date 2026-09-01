// #region MODULE_CONTRACT
// PURPOSE: Preserves modem settings so recovery can clear them independently.
// SCOPE:
// - Reads, validates, migrates, and writes the modem-source profile in appcfg.
// - NOT: Runtime modem control, SMS polling, and HTTP rendering.
// INVARIANTS:
// - Credentials are stored and retrieved only as whole checksummed records.
// - Schema changes must migrate stored data (see AGENTS.md).
// DEPENDENCIES: Uses Preferences partition label appcfg.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_modem.h"
#include "persistence/config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kModemNamespace[] = "modem";
}  // namespace

// #region FUNC_buildModemSourceRecord
// PURPOSE: Gives persistence one bounded modem-source representation shared with forms.
ModemSourceRecord buildModemSourceRecord(const RuntimeModemSourceConfig& config) {
  ModemSourceRecord record{};
  record.magic = kModemSourceMagic;
  record.version = kModemSourceVersion;
  record.moduleEnabled = config.moduleEnabled ? 1 : 0;
  record.pollEnabled = config.pollEnabled ? 1 : 0;
  record.pollIntervalSec = config.pollIntervalSec;
  config.label.toCharArray(record.label, sizeof(record.label));
  record.nitzTimeSyncEnabled = config.nitzTimeSyncEnabled ? 1 : 0;
  record.smsPollEnabled = config.smsPollEnabled ? 1 : 0;
  record.checksum = calculateModemSourceChecksum(record);
  return record;
}
// #endregion FUNC_buildModemSourceRecord

// #region METHOD_ModemSourceStore_migrateV2Record
// PURPOSE: Upgrades a valid legacy modem blob without losing its settings.
bool ModemSourceStore::migrateV2Record(size_t readLength) const {
  if (readLength != sizeof(ModemSourceRecordV2)) return false;
  Preferences preferences;
  if (!preferences.begin(kModemNamespace, true, kAppCfgPartition)) return false;
  ModemSourceRecordV2 legacy{};
  const size_t legacyLength = preferences.getBytes(kAppCfgKey, &legacy, sizeof(legacy));
  preferences.end();
  if (legacyLength != sizeof(legacy) || !isModemSourceRecordV2Valid(legacy)) return false;

  RuntimeModemSourceConfig migrated;
  migrated.moduleEnabled = legacy.enabled == 1;
  migrated.pollEnabled = legacy.enabled == 1;
  migrated.pollIntervalSec = legacy.pollIntervalSec;
  migrated.label = legacy.label;
  migrated.nitzTimeSyncEnabled = legacy.nitzTimeSyncEnabled == 1;
  migrated.smsPollEnabled = legacy.enabled == 1;
  const ModemSourceRecord record = buildModemSourceRecord(migrated);
  if (!isModemSourceRecordValid(record)) return false;
  if (!preferences.begin(kModemNamespace, false, kAppCfgPartition)) {
    Serial.println("event=modem_source_load_migrated persisted=false");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool persisted = writtenLength == sizeof(record);
  Serial.printf("event=modem_source_load_migrated from_version=2 persisted=%s\n",
                persisted ? "true" : "false");
  return persisted;
}
// #endregion METHOD_ModemSourceStore_migrateV2Record

// #region METHOD_ModemSourceStore_load
// PURPOSE: Keeps corrupt or partial modem policy out of the SMS poll path.
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
    if (migrateV2Record(readLength)) return load(config);
    Serial.printf("event=modem_source_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }
  if (!isModemSourceRecordValid(record)) {
    Serial.printf("event=modem_source_load_empty_or_invalid bytes=%u\n",
                  static_cast<unsigned>(readLength));
    return false;
  }

  config.moduleEnabled = record.moduleEnabled == 1;
  config.pollEnabled = record.pollEnabled == 1;
  config.pollIntervalSec = record.pollIntervalSec;
  config.label = record.label;
  config.nitzTimeSyncEnabled = record.nitzTimeSyncEnabled == 1;
  config.smsPollEnabled = record.smsPollEnabled == 1;
  Serial.printf(
      "event=modem_source_load_complete module=%s poll=%s poll_interval=%u nitz=%s sms_poll=%s\n",
      config.moduleEnabled ? "true" : "false", config.pollEnabled ? "true" : "false",
      static_cast<unsigned>(config.pollIntervalSec), config.nitzTimeSyncEnabled ? "true" : "false",
      config.smsPollEnabled ? "true" : "false");
  return true;
}
// #endregion METHOD_ModemSourceStore_load

// #region METHOD_ModemSourceStore_save
// PURPOSE: Commits only shared-valid modem policy so forms and NVS cannot diverge.
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
// #endregion METHOD_ModemSourceStore_save
