// #region MODULE_CONTRACT
// PURPOSE: Defines the portable, checksummed binary record for the ZTE
// MF79RU SMS source (see ADR-0003) so it can be validated independently of
// Arduino hardware APIs.
// INVARIANTS: A record is valid only with the expected magic, version, and
// checksum; host and password are complete printable-ASCII fields so
// corrupt NVS content can never reach the modem dialog.
// #endregion MODULE_CONTRACT

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxZteHostLength = 63;
constexpr size_t kMaxZtePasswordLength = 63;
constexpr uint32_t kZteConfigMagic = 0x5A544547;  // "ZTEG"
constexpr uint16_t kZteConfigVersion = 1;

// #region CLASS_ZteConfigRecord
// PURPOSE: Represents the ZTE modem SMS source profile (enable flag, LAN
// host, and the modem's own web-login password) as a single NVS blob,
// independent of the Wi-Fi and SMTP records.
struct ZteConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  char host[kMaxZteHostLength + 1];
  char password[kMaxZtePasswordLength + 1];
  uint32_t checksum;
};
// #endregion CLASS_ZteConfigRecord

// #region FUNC_calculateZteConfigChecksum
// PURPOSE: Detects incomplete and incompatible records before the modem
// credentials can be used by the device.
inline uint32_t calculateZteConfigChecksum(const ZteConfigRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecord, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateZteConfigChecksum

// #region FUNC_isZteConfigRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content
// and web input obey the same rules; credentials must be complete even when
// the source is disabled, so re-enabling never needs re-entry.
inline bool isZteConfigRecordValid(const ZteConfigRecord& record) {
  if (record.magic != kZteConfigMagic || record.version != kZteConfigVersion ||
      record.checksum != calculateZteConfigChecksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) {
    return false;
  }
  if (!codec::isPrintableRange(record.host, kMaxZteHostLength) || record.host[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.password, kMaxZtePasswordLength) ||
      record.password[0] == '\0') {
    return false;
  }
  return true;
}
// #endregion FUNC_isZteConfigRecordValid
