// #region MODULE_CONTRACT
// PURPOSE: Proves persisted profiles migrate without silently truncating runtime values.
// SCOPE:
// - Exercises GPS v2, modem v2, and ZTE v3 in-place migrations through their NVS stores.
// - Verifies all text persistence stores reject source fields larger than their record capacity.
// - NOT: Hardware NVS behavior or web configuration.
// #endregion MODULE_CONTRACT

#include <assert.h>
#include <string.h>

#include <string>
#include <vector>

#include <Preferences.h>

#include "../sms_gate/persistence/config_store_gps.h"
#include "../sms_gate/persistence/config_store_modem.h"
#include "../sms_gate/persistence/config_store_network.h"
#include "../sms_gate/persistence/config_store_smtp.h"
#include "../sms_gate/persistence/config_store_zte.h"

namespace {

// #region FUNC_repeatedString
// PURPOSE: Produces exact source-length boundaries for persistence rejection tests.
String repeatedString(size_t length) { return String(std::string(length, 'x').c_str()); }
// #endregion FUNC_repeatedString

// #region FUNC_assertStoredRecordUnchanged
// PURPOSE: Confirms a rejected save cannot replace the last valid NVS record.
void assertStoredRecordUnchanged(const std::vector<uint8_t>& expected) {
  assert(Preferences::bytes() == expected);
}
// #endregion FUNC_assertStoredRecordUnchanged

}  // namespace

// ConfigStore::load references this production validator; save tests below do not.
// #region FUNC_isValidPassword
// PURPOSE: Satisfies the unrelated load-path link dependency in this save-focused host test.
bool isValidPassword(const String&) { return false; }
// #endregion FUNC_isValidPassword

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

// #region FUNC_testTextStoreRejectsOversizeSourceFields
// PURPOSE: Prevents String::toCharArray truncation from being checksummed and persisted.
void testTextStoreRejectsOversizeSourceFields() {
  RuntimeConfig network;
  network.ssid = "test-network";
  network.wifiPassword = "network-password";
  network.adminPassword = "admin-password";
  network.ntpServer1 = "pool.ntp.org";
  network.ntpServer2 = "time.nist.gov";
  ConfigStore networkStore;
  assert(networkStore.save(network));
  const std::vector<uint8_t> networkRecord = Preferences::bytes();
  network.ssid = repeatedString(sizeof(ConfigRecord::ssid));
  assert(!networkStore.save(network));
  assertStoredRecordUnchanged(networkRecord);
  network.ssid = "test-network";
  network.wifiPassword = repeatedString(sizeof(ConfigRecord::wifiPassword));
  assert(!networkStore.save(network));
  assertStoredRecordUnchanged(networkRecord);
  network.wifiPassword = "network-password";
  network.adminPassword = repeatedString(sizeof(ConfigRecord::adminPassword));
  assert(!networkStore.save(network));
  assertStoredRecordUnchanged(networkRecord);
  network.adminPassword = "admin-password";
  network.ntpServer1 = repeatedString(sizeof(ConfigRecord::ntpServer1));
  assert(!networkStore.save(network));
  assertStoredRecordUnchanged(networkRecord);
  network.ntpServer1 = "pool.ntp.org";
  network.ntpServer2 = repeatedString(sizeof(ConfigRecord::ntpServer2));
  assert(!networkStore.save(network));
  assertStoredRecordUnchanged(networkRecord);

  RuntimeSmtpConfig smtp;
  smtp.host = "smtp.example.com";
  smtp.username = "user@example.com";
  smtp.password = "smtp-password";
  smtp.fromAddress = "device@example.com";
  smtp.recipientAddress = "owner@example.com";
  SmtpConfigStore smtpStore;
  assert(smtpStore.save(smtp));
  const std::vector<uint8_t> smtpRecord = Preferences::bytes();
  smtp.host = repeatedString(sizeof(SmtpConfigRecord::host));
  assert(!smtpStore.save(smtp));
  assertStoredRecordUnchanged(smtpRecord);
  smtp.host = "smtp.example.com";
  smtp.username = repeatedString(sizeof(SmtpConfigRecord::username));
  assert(!smtpStore.save(smtp));
  assertStoredRecordUnchanged(smtpRecord);
  smtp.username = "user@example.com";
  smtp.password = repeatedString(sizeof(SmtpConfigRecord::password));
  assert(!smtpStore.save(smtp));
  assertStoredRecordUnchanged(smtpRecord);
  smtp.password = "smtp-password";
  smtp.fromAddress = repeatedString(sizeof(SmtpConfigRecord::fromAddress));
  assert(!smtpStore.save(smtp));
  assertStoredRecordUnchanged(smtpRecord);
  smtp.fromAddress = "device@example.com";
  smtp.recipientAddress = repeatedString(sizeof(SmtpConfigRecord::recipientAddress));
  assert(!smtpStore.save(smtp));
  assertStoredRecordUnchanged(smtpRecord);

  RuntimeZteConfig zte;
  zte.host = "192.168.1.1";
  zte.password = "modem-password";
  zte.label = "modem";
  ZteConfigStore zteStore;
  assert(zteStore.save(zte));
  const std::vector<uint8_t> zteRecord = Preferences::bytes();
  zte.host = repeatedString(sizeof(ZteConfigRecord::host));
  assert(!zteStore.save(zte));
  assertStoredRecordUnchanged(zteRecord);
  zte.host = "192.168.1.1";
  zte.password = repeatedString(sizeof(ZteConfigRecord::password));
  assert(!zteStore.save(zte));
  assertStoredRecordUnchanged(zteRecord);
  zte.password = "modem-password";
  zte.label = repeatedString(sizeof(ZteConfigRecord::label));
  assert(!zteStore.save(zte));
  assertStoredRecordUnchanged(zteRecord);

  RuntimeModemSourceConfig modem;
  modem.label = "modem";
  ModemSourceStore modemStore;
  assert(modemStore.save(modem));
  const std::vector<uint8_t> modemRecord = Preferences::bytes();
  modem.label = repeatedString(sizeof(ModemSourceRecord::label));
  assert(!modemStore.save(modem));
  assertStoredRecordUnchanged(modemRecord);
}
// #endregion FUNC_testTextStoreRejectsOversizeSourceFields

int main() {
  testGpsV2MigrationAtEqualLength();
  testModemV2MigrationAtEqualLength();
  testZteV3MigrationAtEqualLength();
  testTextStoreRejectsOversizeSourceFields();
  return 0;
}
