// #region MODULE_CONTRACT
// PURPOSE: Implements GpsConfigStore so the sketch only drives load/save.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_gps.h"
#include "persistence/config_store_common.h"

#include <Preferences.h>

namespace {
constexpr char kGpsNamespace[] = "gps";
}  // namespace

// #region FUNC_buildGpsRecord
// PURPOSE: Converts runtime GNSS profile into checksummed binary record.
GpsRecord buildGpsRecord(const RuntimeGpsConfig& config) {
  GpsRecord record{};
  record.magic = kGpsMagic;
  record.version = kGpsVersion;
  record.enabled = config.enabled ? 1 : 0;
  record.pollIntervalSec = config.pollIntervalSec;
  if (!isValidGpsPollInterval(record.pollIntervalSec)) {
    record.pollIntervalSec = kDefaultGpsPollSec;
  }
  record.checksum = calculateGpsChecksum(record);
  return record;
}
// #endregion FUNC_buildGpsRecord

// #region FUNC_GpsConfigStore_load
// PURPOSE: Restores GNSS profile only as whole validated record.
bool GpsConfigStore::load(RuntimeGpsConfig& config) const {
  Serial.printf("event=gps_load_begin partition=%s\n", kAppCfgPartition);
  Preferences preferences;
  if (!preferences.begin(kGpsNamespace, true, kAppCfgPartition)) {
    Serial.println("event=gps_load_failed reason=partition_unavailable");
    return false;
  }
  GpsRecord record{};
  const size_t readLength = preferences.getBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  if (readLength != sizeof(record) || !isGpsRecordValid(record)) {
    Serial.printf("event=gps_load_empty_or_invalid bytes=%u\n", static_cast<unsigned>(readLength));
    return false;
  }
  config.enabled = record.enabled == 1;
  config.pollIntervalSec = record.pollIntervalSec;
  Serial.printf("event=gps_load_complete enabled=%s poll_interval=%u\n",
                config.enabled ? "true" : "false", static_cast<unsigned>(config.pollIntervalSec));
  return true;
}
// #endregion FUNC_GpsConfigStore_load

// #region FUNC_GpsConfigStore_save
// PURPOSE: Persists GNSS profile only after validation.
bool GpsConfigStore::save(const RuntimeGpsConfig& config) const {
  Serial.println("event=gps_save_begin");
  const GpsRecord record = buildGpsRecord(config);
  if (!isGpsRecordValid(record)) {
    Serial.println("event=gps_save_failed reason=invalid_fields");
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kGpsNamespace, false, kAppCfgPartition)) {
    Serial.println("event=gps_save_failed reason=partition_unavailable");
    return false;
  }
  const size_t writtenLength = preferences.putBytes(kAppCfgKey, &record, sizeof(record));
  preferences.end();
  const bool saved = writtenLength == sizeof(record);
  Serial.printf("event=gps_save_complete saved=%s bytes=%u\n", saved ? "true" : "false",
                static_cast<unsigned>(writtenLength));
  return saved;
}
// #endregion FUNC_GpsConfigStore_save
