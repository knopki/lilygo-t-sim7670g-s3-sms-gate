#include <assert.h>
#include <string.h>

#include "../sms_gate/persistence/config_record.h"

int main() {
  ConfigRecord record{};
  record.magic = kConfigMagic;
  record.version = kConfigVersion;
  strcpy(record.ssid, "test-network");
  strcpy(record.wifiPassword, "network-password");
  strcpy(record.adminPassword, "admin-password");
  record.checksum = calculateConfigChecksum(record);

  assert(record.checksum == calculateConfigChecksum(record));
  record.wifiPassword[0] = 'N';
  assert(record.checksum != calculateConfigChecksum(record));
  assert(kMaxSsidLength == 32);
  assert(kMinPasswordLength == 8);
  assert(kMaxPasswordLength == 63);
  return 0;
}
