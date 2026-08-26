#include <assert.h>
#include <string.h>

#include "../sms_gate/persistence/config_record.h"
#include "../sms_gate/gps/gps_record.h"
#include "../sms_gate/modem/modem_record.h"

static ConfigRecord makeRecord() {
  ConfigRecord r{};
  r.magic = kConfigMagic;
  r.version = kConfigVersion;
  strcpy(r.ssid, "test-network");
  strcpy(r.wifiPassword, "network-password");
  strcpy(r.adminPassword, "admin-password");
  strcpy(r.ntpServer1, "pool.ntp.org");
  strcpy(r.ntpServer2, "time.nist.gov");
  r.ntpEnabled = 1;
  r.checksum = calculateConfigChecksum(r);
  return r;
}

static ConfigRecordV1 makeV1() {
  ConfigRecordV1 r{};
  r.magic = kConfigMagic;
  r.version = 1;
  strcpy(r.ssid, "test-network");
  strcpy(r.wifiPassword, "network-password");
  strcpy(r.adminPassword, "admin-password");
  r.checksum = calculateConfigV1Checksum(r);
  return r;
}

int main() {
  // v2 happy path
  ConfigRecord record = makeRecord();
  assert(isConfigRecordValid(record));
  assert(record.checksum == calculateConfigChecksum(record));
  record.wifiPassword[0] = 'N';
  assert(record.checksum != calculateConfigChecksum(record));
  assert(kMaxSsidLength == 32);
  assert(kMinPasswordLength == 8);
  assert(kMaxPasswordLength == 63);
  assert(kMaxNtpServerLength == 64);

  // ntpEnabled 0/1 valid
  record = makeRecord();
  record.ntpEnabled = 0;
  record.checksum = calculateConfigChecksum(record);
  assert(isConfigRecordValid(record));
  record.ntpEnabled = 2;
  record.checksum = calculateConfigChecksum(record);
  assert(!isConfigRecordValid(record));

  // empty ntp servers valid (disabled case)
  record = makeRecord();
  record.ntpServer1[0] = '\0';
  record.ntpServer2[0] = '\0';
  record.ntpEnabled = 0;
  record.checksum = calculateConfigChecksum(record);
  assert(isConfigRecordValid(record));

  // non-printable ntp server rejected
  record = makeRecord();
  strcpy(record.ntpServer1, "bad\x01host");
  record.checksum = calculateConfigChecksum(record);
  assert(!isConfigRecordValid(record));

  // unterminated field rejected
  record = makeRecord();
  memset(record.ssid, 'A', sizeof(record.ssid));
  record.checksum = calculateConfigChecksum(record);
  assert(!isConfigRecordValid(record));

  // version/magic/ checksum
  record = makeRecord();
  record.version = 99;
  assert(!isConfigRecordValid(record));
  record = makeRecord();
  record.magic = 0xDEADBEEF;
  assert(!isConfigRecordValid(record));
  record = makeRecord();
  record.checksum ^= 1;
  assert(!isConfigRecordValid(record));

  // v1 happy path
  ConfigRecordV1 v1 = makeV1();
  assert(isConfigRecordV1Valid(v1));
  v1.wifiPassword[0] = 'X';
  assert(v1.checksum != calculateConfigV1Checksum(v1));
  // v1 bad version
  v1 = makeV1();
  v1.version = 2;
  assert(!isConfigRecordV1Valid(v1));

  // GpsRecord v3: module + poll + timeSync
  GpsRecord g{};
  g.magic = kGpsMagic;
  g.version = kGpsVersion;
  g.moduleEnabled = 1;
  g.pollEnabled = 1;
  g.pollIntervalSec = kDefaultGpsPollSec;
  g.timeSyncEnabled = 1;
  g.checksum = calculateGpsChecksum(g);
  assert(isGpsRecordValid(g));
  g.timeSyncEnabled = 0;
  g.checksum = calculateGpsChecksum(g);
  assert(isGpsRecordValid(g));
  g.timeSyncEnabled = 2;
  g.checksum = calculateGpsChecksum(g);
  assert(!isGpsRecordValid(g));
  g = GpsRecord{};
  g.magic = kGpsMagic;
  g.version = kGpsVersion;
  g.moduleEnabled = 1;
  g.pollEnabled = 0;
  g.pollIntervalSec = kDefaultGpsPollSec;
  g.timeSyncEnabled = 0;
  g.checksum = calculateGpsChecksum(g);
  assert(isGpsRecordValid(g));
  g.pollEnabled = 2;
  g.checksum = calculateGpsChecksum(g);
  assert(!isGpsRecordValid(g));
  GpsRecordV2 gv2{};
  gv2.magic = kGpsMagic;
  gv2.version = 2;
  gv2.enabled = 0;
  gv2.pollIntervalSec = kDefaultGpsPollSec;
  gv2.timeSyncEnabled = 1;
  gv2.checksum = calculateGpsV2Checksum(gv2);
  assert(isGpsRecordV2Valid(gv2));

  // Modem v3: module + poll + sms + nitz
  ModemSourceRecord m{};
  m.magic = kModemSourceMagic;
  m.version = kModemSourceVersion;
  m.moduleEnabled = 1;
  m.pollEnabled = 1;
  m.pollIntervalSec = kDefaultModemPollSec;
  strcpy(m.label, "test");
  m.nitzTimeSyncEnabled = 1;
  m.smsPollEnabled = 1;
  m.checksum = calculateModemSourceChecksum(m);
  assert(isModemSourceRecordValid(m));
  m.smsPollEnabled = 2;
  m.checksum = calculateModemSourceChecksum(m);
  assert(!isModemSourceRecordValid(m));
  m.smsPollEnabled = 1;
  m.moduleEnabled = 2;
  m.checksum = calculateModemSourceChecksum(m);
  assert(!isModemSourceRecordValid(m));

  return 0;
}
