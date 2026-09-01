// #region MODULE_CONTRACT
// PURPOSE: Proves legacy blobs migrate when their byte length matches the current schema.
// SCOPE:
// - Exercises GPS v2, modem v2, and ZTE v3 in-place migrations through their NVS stores.
// - NOT: Hardware NVS behavior or web configuration.
// #endregion MODULE_CONTRACT

#include <assert.h>
#include <string.h>

#include <Preferences.h>

#include "../sms_gate/persistence/config_store_gps.h"
#include "../sms_gate/persistence/config_store_modem.h"
#include "../sms_gate/persistence/config_store_zte.h"

// #region FUNC_testGpsV2MigrationAtEqualLength
// PURPOSE: Preserves GNSS settings when v2 and v3 records occupy the same blob length.
void testGpsV2MigrationAtEqualLength() {
  static_assert(sizeof(GpsRecord) == sizeof(GpsRecordV2));
  GpsRecordV2 legacy{};
  legacy.magic = kGpsMagic;
  legacy.version = 2;
  legacy.enabled = 1;
  legacy.pollIntervalSec = 75;
  legacy.timeSyncEnabled = 1;
  legacy.checksum = calculateGpsV2Checksum(legacy);
  Preferences::setBytes(&legacy, sizeof(legacy));

  RuntimeGpsConfig config;
  assert(GpsConfigStore().load(config));
  assert(config.moduleEnabled);
  assert(config.pollEnabled);
  assert(config.pollIntervalSec == legacy.pollIntervalSec);
  assert(config.timeSyncEnabled);

  GpsRecord migrated{};
  memcpy(&migrated, Preferences::bytes().data(), sizeof(migrated));
  assert(isGpsRecordValid(migrated));
}
// #endregion FUNC_testGpsV2MigrationAtEqualLength

// #region FUNC_testModemV2MigrationAtEqualLength
// PURPOSE: Preserves modem settings when v2 and v3 records occupy the same blob length.
void testModemV2MigrationAtEqualLength() {
  static_assert(sizeof(ModemSourceRecord) == sizeof(ModemSourceRecordV2));
  ModemSourceRecordV2 legacy{};
  legacy.magic = kModemSourceMagic;
  legacy.version = 2;
  legacy.enabled = 1;
  legacy.pollIntervalSec = 75;
  strcpy(legacy.label, "+79990000000");
  legacy.nitzTimeSyncEnabled = 1;
  legacy.checksum = calculateModemSourceV2Checksum(legacy);
  Preferences::setBytes(&legacy, sizeof(legacy));

  RuntimeModemSourceConfig config;
  assert(ModemSourceStore().load(config));
  assert(config.moduleEnabled);
  assert(config.pollEnabled);
  assert(config.pollIntervalSec == legacy.pollIntervalSec);
  assert(config.label == legacy.label);
  assert(config.nitzTimeSyncEnabled);
  assert(config.smsPollEnabled);

  ModemSourceRecord migrated{};
  memcpy(&migrated, Preferences::bytes().data(), sizeof(migrated));
  assert(isModemSourceRecordValid(migrated));
}
// #endregion FUNC_testModemV2MigrationAtEqualLength

// #region FUNC_testZteV3MigrationAtEqualLength
// PURPOSE: Preserves ZTE settings when v3 and v4 records occupy the same blob length.
void testZteV3MigrationAtEqualLength() {
  static_assert(sizeof(ZteConfigRecord) == sizeof(ZteConfigRecordV3));
  ZteConfigRecordV3 legacy{};
  legacy.magic = kZteConfigMagic;
  legacy.version = 3;
  legacy.enabled = 1;
  strcpy(legacy.host, "192.168.1.1");
  strcpy(legacy.password, "modem-password");
  strcpy(legacy.label, "+79990000000");
  legacy.pollIntervalSec = 75;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&legacy);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecordV3, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  legacy.checksum = hash;
  Preferences::setBytes(&legacy, sizeof(legacy));

  RuntimeZteConfig config;
  assert(ZteConfigStore().load(config));
  assert(config.moduleEnabled);
  assert(config.forwardEnabled);
  assert(config.host == legacy.host);
  assert(config.password == legacy.password);
  assert(config.label == legacy.label);
  assert(config.pollIntervalSec == legacy.pollIntervalSec);

  ZteConfigRecord migrated{};
  memcpy(&migrated, Preferences::bytes().data(), sizeof(migrated));
  assert(isZteConfigRecordValid(migrated));
}
// #endregion FUNC_testZteV3MigrationAtEqualLength

int main() {
  testGpsV2MigrationAtEqualLength();
  testModemV2MigrationAtEqualLength();
  testZteV3MigrationAtEqualLength();
  return 0;
}
