// #region MODULE_CONTRACT
// PURPOSE: Keeps ZTE settings verifiable and independent of hardware APIs.
// SCOPE:
// - Defines ZTE record layouts, checksums, and validation predicates.
// - NOT: NVS access, ZTE modem dialogs, SMS forwarding, and HTTP rendering.
// INVARIANTS:
// - A record is valid only with the expected magic, version, and checksum;
// - host and password are complete printable-ASCII fields so corrupt NVS content
//   can never reach the modem dialog.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_RECORD_H
#define ZTE_ZTE_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxZteHostLength = 63;
constexpr size_t kMaxZtePasswordLength = 63;
constexpr size_t kMaxZteLabelLength = 31;
constexpr uint16_t kDefaultZtePollSec = 15;
constexpr uint16_t kMinZtePollSec = 5;
constexpr uint16_t kMaxZtePollSec = 300;
constexpr uint32_t kZteConfigMagic = 0x5A544547;  // "ZTEG"
// Version history: 1 lacked the source label; 2 added label; 3 added
// per-source poll interval; 4 split enabled into moduleEnabled + forwardEnabled.
constexpr uint16_t kZteConfigVersion = 4;

// #region FUNC_isValidZtePollInterval
// PURPOSE: Centralizes the per-source ZTE poll interval contract (5..300 s).
inline bool isValidZtePollInterval(uint16_t value) {
  return value >= kMinZtePollSec && value <= kMaxZtePollSec;
}
// #endregion FUNC_isValidZtePollInterval

// #region STRUCT_ZteConfigRecord
// PURPOSE: Represents the ZTE modem SMS source profile (module enable,
// LAN host, the modem's own web-login password, phone number or alias shown
// in forwarded emails, per-source poll interval, and forward enable) as a
// single NVS blob, independent of the Wi-Fi and SMTP records.
struct ZteConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t moduleEnabled;
  uint8_t forwardEnabled;
  char host[kMaxZteHostLength + 1];
  char password[kMaxZtePasswordLength + 1];
  char label[kMaxZteLabelLength + 1];
  uint16_t pollIntervalSec;
  uint32_t checksum;
};
// #endregion STRUCT_ZteConfigRecord

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
  if (record.moduleEnabled != 0 && record.moduleEnabled != 1) {
    return false;
  }
  if (record.forwardEnabled != 0 && record.forwardEnabled != 1) {
    return false;
  }
  if (!codec::isPrintableRange(record.host, kMaxZteHostLength) || record.host[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.password, kMaxZtePasswordLength) ||
      record.password[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.label, kMaxZteLabelLength)) {
    return false;
  }
  return isValidZtePollInterval(record.pollIntervalSec);
}
// #endregion FUNC_isZteConfigRecordValid

// #region STRUCT_ZteConfigRecordV3
// PURPOSE: Preserves v3 layout (single enabled flag + poll interval) for migration to v4 — sample
// kept, older removed.
struct ZteConfigRecordV3 {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  char host[kMaxZteHostLength + 1];
  char password[kMaxZtePasswordLength + 1];
  char label[kMaxZteLabelLength + 1];
  uint16_t pollIntervalSec;
  uint32_t checksum;
};
// #endregion STRUCT_ZteConfigRecordV3

// #region FUNC_isZteConfigRecordV3Valid
// PURPOSE: Rejects corrupt legacy ZTE profiles before migration.
inline bool isZteConfigRecordV3Valid(const ZteConfigRecordV3& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecordV3, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  if (record.magic != kZteConfigMagic || record.version != 3 || record.checksum != hash) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) return false;
  if (!codec::isPrintableRange(record.host, kMaxZteHostLength) || record.host[0] == '\0')
    return false;
  if (!codec::isPrintableRange(record.password, kMaxZtePasswordLength) ||
      record.password[0] == '\0')
    return false;
  if (!codec::isPrintableRange(record.label, kMaxZteLabelLength)) return false;
  return isValidZtePollInterval(record.pollIntervalSec);
}
// #endregion FUNC_isZteConfigRecordV3Valid

#endif  // ZTE_ZTE_RECORD_H
