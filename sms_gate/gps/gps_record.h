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
constexpr uint16_t kGpsVersion = 2;
// Version history: 1 = enabled + pollIntervalSec;
// 2 = added timeSyncEnabled (ADR-0005).
constexpr uint16_t kDefaultGpsPollSec = 60;
constexpr uint16_t kMinGpsPollSec = 5;
constexpr uint16_t kMaxGpsPollSec = 300;

// #region STRUCT_GpsRecord
// PURPOSE: Represents the GNSS polling profile (enable flag, poll interval
// and time-sync enable) as a single NVS blob under appcfg/gps.
struct GpsRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint16_t pollIntervalSec;
  uint8_t timeSyncEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_GpsRecord

// #region STRUCT_GpsRecordV1
// PURPOSE: Preserves v1 layout for migration (without timeSyncEnabled).
struct GpsRecordV1 {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint16_t pollIntervalSec;
  uint32_t checksum;
};
// #endregion STRUCT_GpsRecordV1

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

// #region FUNC_calculateGpsV1Checksum
inline uint32_t calculateGpsV1Checksum(const GpsRecordV1& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(GpsRecordV1, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateGpsV1Checksum

inline bool isValidGpsPollInterval(uint16_t value) {
  return value >= kMinGpsPollSec && value <= kMaxGpsPollSec;
}

// #region FUNC_isGpsRecordV1Valid
inline bool isGpsRecordV1Valid(const GpsRecordV1& record) {
  if (record.magic != kGpsMagic || record.version != 1 ||
      record.checksum != calculateGpsV1Checksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) return false;
  if (!isValidGpsPollInterval(record.pollIntervalSec)) return false;
  return true;
}
// #endregion FUNC_isGpsRecordV1Valid

// #region FUNC_isGpsRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content
// and web input obey the same rules.
inline bool isGpsRecordValid(const GpsRecord& record) {
  if (record.magic != kGpsMagic || record.version != kGpsVersion ||
      record.checksum != calculateGpsChecksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) {
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
