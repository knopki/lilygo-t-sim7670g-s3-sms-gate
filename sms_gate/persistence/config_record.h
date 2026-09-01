// #region MODULE_CONTRACT
// PURPOSE: Keeps the saved network profile verifiable across recovery and boots.
// SCOPE:
// - Defines network record layouts, checksums, and validation predicates.
// - NOT: NVS access, Wi-Fi connection management, and HTTP authentication.
// INVARIANTS: A record is valid only with the expected magic, version, and checksum.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_RECORD_H
#define PERSISTENCE_CONFIG_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 63;
constexpr size_t kMaxNtpServerLength = 64;
constexpr uint32_t kConfigMagic = 0x534D5347;  // "SMSG"
constexpr uint16_t kConfigVersion = 2;
// Version history: 1 = ssid/wifiPassword/adminPassword only;
// 2 = added ntpServer1/2 + ntpEnabled (ADR-0005).

// #region STRUCT_ConfigRecord
// PURPOSE: Represents one complete network and administrator configuration as
// a single NVS blob (v2).
struct ConfigRecord {
  uint32_t magic;
  uint16_t version;
  char ssid[kMaxSsidLength + 1];
  char wifiPassword[kMaxPasswordLength + 1];
  char adminPassword[kMaxPasswordLength + 1];
  char ntpServer1[kMaxNtpServerLength + 1];
  char ntpServer2[kMaxNtpServerLength + 1];
  uint8_t ntpEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_ConfigRecord

// #region STRUCT_ConfigRecordV1
// PURPOSE: Preserves the exact v1 layout so load can recognize, validate
// and migrate stored v1 records to v2.
struct ConfigRecordV1 {
  uint32_t magic;
  uint16_t version;
  char ssid[kMaxSsidLength + 1];
  char wifiPassword[kMaxPasswordLength + 1];
  char adminPassword[kMaxPasswordLength + 1];
  uint32_t checksum;
};
// #endregion STRUCT_ConfigRecordV1

// #region FUNC_calculateConfigChecksum
// PURPOSE: Detects incomplete and incompatible records before their credentials
// can be used by the device.
inline uint32_t calculateConfigChecksum(const ConfigRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ConfigRecord, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateConfigChecksum

// #region FUNC_calculateConfigV1Checksum
// PURPOSE: Validates legacy blobs before migration can use their fields.
inline uint32_t calculateConfigV1Checksum(const ConfigRecordV1& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ConfigRecordV1, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateConfigV1Checksum

// #region FUNC_isConfigRecordV1Valid
// PURPOSE: Rejects corrupt legacy profiles before migration.
inline bool isConfigRecordV1Valid(const ConfigRecordV1& record) {
  if (record.magic != kConfigMagic || record.version != 1 ||
      record.checksum != calculateConfigV1Checksum(record)) {
    return false;
  }
  // ssid may be empty (initial setup) — only check NUL-termination via printable range
  // but allow empty string; wifi/admin passwords checked at higher layer.
  if (!codec::isPrintableRange(record.ssid, kMaxSsidLength)) return false;
  if (!codec::isPrintableRange(record.wifiPassword, kMaxPasswordLength)) return false;
  if (!codec::isPrintableRange(record.adminPassword, kMaxPasswordLength)) return false;
  return true;
}
// #endregion FUNC_isConfigRecordV1Valid

// #region FUNC_isConfigRecordValid
// PURPOSE: Rejects corrupt current profiles before credentials are used.
inline bool isConfigRecordValid(const ConfigRecord& record) {
  if (record.magic != kConfigMagic || record.version != kConfigVersion ||
      record.checksum != calculateConfigChecksum(record)) {
    return false;
  }
  if (!codec::isPrintableRange(record.ssid, kMaxSsidLength)) return false;
  if (!codec::isPrintableRange(record.wifiPassword, kMaxPasswordLength)) return false;
  if (!codec::isPrintableRange(record.adminPassword, kMaxPasswordLength)) return false;
  if (!codec::isPrintableRange(record.ntpServer1, kMaxNtpServerLength)) return false;
  if (!codec::isPrintableRange(record.ntpServer2, kMaxNtpServerLength)) return false;
  if (record.ntpEnabled != 0 && record.ntpEnabled != 1) return false;
  return true;
}
// #endregion FUNC_isConfigRecordValid

#endif  // PERSISTENCE_CONFIG_RECORD_H
