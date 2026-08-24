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
constexpr size_t kMaxZteLabelLength = 31;
constexpr uint32_t kZteConfigMagic = 0x5A544547;  // "ZTEG"
// Version history: 1 lacked the source label; 2 = current layout after the
// per-source phone-number/alias field was added. Load migrates stored
// v1 data (see config_store.cpp).
constexpr uint16_t kZteConfigVersion = 2;

// #region CLASS_ZteConfigRecord
// PURPOSE: Represents the ZTE modem SMS source profile (enable flag, LAN
// host, the modem's own web-login password, and the phone number or alias
// shown in forwarded emails) as a single NVS blob, independent of the
// Wi-Fi and SMTP records.
struct ZteConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  char host[kMaxZteHostLength + 1];
  char password[kMaxZtePasswordLength + 1];
  char label[kMaxZteLabelLength + 1];
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
// the source is disabled, so re-enabling never needs re-entry. The label is
// optional and may stay empty.
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
  return codec::isPrintableRange(record.label, kMaxZteLabelLength);
}
// #endregion FUNC_isZteConfigRecordValid

// #region STRUCT_ZteConfigRecordV1
// PURPOSE: Preserves the exact v1 layout so load can recognize, validate,
// and migrate stored pre-label records instead of dropping them.
struct ZteConfigRecordV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  char host[kMaxZteHostLength + 1];
  char password[kMaxZtePasswordLength + 1];
  uint32_t checksum;
};
// #endregion STRUCT_ZteConfigRecordV1

// #region FUNC_isZteConfigRecordV1Valid
// PURPOSE: Validates a stored v1 record against its own original layout and
// checksum before its fields are carried into a v2 record.
inline bool isZteConfigRecordV1Valid(const ZteConfigRecordV1& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecordV1, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  if (record.magic != kZteConfigMagic || record.version != 1 || record.checksum != hash) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) {
    return false;
  }
  if (!codec::isPrintableRange(record.host, kMaxZteHostLength) || record.host[0] == '\0') {
    return false;
  }
  return codec::isPrintableRange(record.password, kMaxZtePasswordLength) &&
         record.password[0] != '\0';
}
// #endregion FUNC_isZteConfigRecordV1Valid
