// #region MODULE_CONTRACT
// PURPOSE: Defines the portable, checksummed binary record for the GPS/GNSS
// polling feature (SIM7670G internal GNSS) so it can be validated independently
// of Arduino hardware APIs.
// INVARIANTS: A record is valid only with expected magic, version, checksum,
// and poll interval 5..300 seconds.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef GPS_GPS_RECORD_H
#define GPS_GPS_RECORD_H

#include <stddef.h>
#include <stdint.h>

constexpr uint32_t kGpsMagic = 0x47505343;  // "GPSC"
constexpr uint16_t kGpsVersion = 3;
// Version history: 1 = enabled + pollIntervalSec;
// 2 = added timeSyncEnabled (ADR-0005);
// 3 = split enabled into moduleEnabled + pollEnabled (pollEnabled gated by module).
constexpr uint16_t kDefaultGpsPollSec = 60;
constexpr uint16_t kMinGpsPollSec = 5;
constexpr uint16_t kMaxGpsPollSec = 300;

// #region STRUCT_GpsRecord
// PURPOSE: Represents the GNSS polling profile (module enable, poll enable,
// poll interval and time-sync enable) as a single NVS blob under appcfg/gps.
struct GpsRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t moduleEnabled;
  uint8_t pollEnabled;
  uint16_t pollIntervalSec;
  uint8_t timeSyncEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_GpsRecord

// #region STRUCT_GpsRecordV2
// PURPOSE: Preserves v2 layout for migration (single enabled flag) — sample kept, older removed.
struct GpsRecordV2 {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint16_t pollIntervalSec;
  uint8_t timeSyncEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_GpsRecordV2

// #region FUNC_calculateGpsChecksum
// PURPOSE: Detects incomplete and incompatible records before GNSS polling
// can use the stored profile.
inline uint32_t calculateGpsChecksum(const GpsRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(GpsRecord, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateGpsChecksum

inline uint32_t calculateGpsV2Checksum(const GpsRecordV2& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(GpsRecordV2, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

inline bool isValidGpsPollInterval(uint16_t value) {
  return value >= kMinGpsPollSec && value <= kMaxGpsPollSec;
}

// #region FUNC_isGpsRecordV2Valid
inline bool isGpsRecordV2Valid(const GpsRecordV2& record) {
  if (record.magic != kGpsMagic || record.version != 2 ||
      record.checksum != calculateGpsV2Checksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) return false;
  if (record.timeSyncEnabled != 0 && record.timeSyncEnabled != 1) return false;
  if (!isValidGpsPollInterval(record.pollIntervalSec)) return false;
  return true;
}
// #endregion FUNC_isGpsRecordV2Valid

// #region FUNC_isGpsRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content
// and web input obey the same rules.
inline bool isGpsRecordValid(const GpsRecord& record) {
  if (record.magic != kGpsMagic || record.version != kGpsVersion ||
      record.checksum != calculateGpsChecksum(record)) {
    return false;
  }
  if (record.moduleEnabled != 0 && record.moduleEnabled != 1) {
    return false;
  }
  if (record.pollEnabled != 0 && record.pollEnabled != 1) {
    return false;
  }
  if (record.timeSyncEnabled != 0 && record.timeSyncEnabled != 1) {
    return false;
  }
  if (!isValidGpsPollInterval(record.pollIntervalSec)) {
    return false;
  }
  return true;
}
// #endregion FUNC_isGpsRecordValid

#endif  // GPS_GPS_RECORD_H
