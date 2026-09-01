// #region MODULE_CONTRACT
// PURPOSE: Keeps modem-source settings verifiable and independent of hardware APIs.
// SCOPE:
// - Defines modem-source record layouts, checksums, and validation predicates.
// - NOT: NVS access, modem hardware I/O, SMS polling, and forwarding.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_RECORD_H
#define MODEM_MODEM_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxModemLabelLength = 31;
constexpr uint32_t kModemSourceMagic = 0x4D44534D;  // "MDSM"
constexpr uint16_t kModemSourceVersion = 3;
// Version history: 1 = enabled + pollIntervalSec + label;
// 2 = added nitzTimeSyncEnabled (ADR-0005);
// 3 = split enabled into moduleEnabled + pollEnabled + smsPollEnabled.
constexpr uint16_t kDefaultModemPollSec = 15;
constexpr uint16_t kMinModemPollSec = 5;
constexpr uint16_t kMaxModemPollSec = 300;

// #region STRUCT_ModemSourceRecord
// PURPOSE: Represents the internal SIM7670G SMS source profile (module enable,
// status poll enable, poll interval, phone number or alias shown in forwarded
// emails, NITZ time-sync enable, and SMS poll/forward enable) as a single
// NVS blob, independent of the Wi-Fi, SMTP, and ZTE records.
struct ModemSourceRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t moduleEnabled;
  uint8_t pollEnabled;
  uint16_t pollIntervalSec;
  char label[kMaxModemLabelLength + 1];
  uint8_t nitzTimeSyncEnabled;
  uint8_t smsPollEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_ModemSourceRecord

// #region STRUCT_ModemSourceRecordV2
// PURPOSE: Preserves v2 layout for migration (single enabled flag + nitz) — sample kept, older
// removed.
struct ModemSourceRecordV2 {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint16_t pollIntervalSec;
  char label[kMaxModemLabelLength + 1];
  uint8_t nitzTimeSyncEnabled;
  uint32_t checksum;
};
// #endregion STRUCT_ModemSourceRecordV2

// #region FUNC_calculateModemSourceChecksum
// PURPOSE: Detects incomplete and incompatible records before the modem
// credentials can be used by the device.
inline uint32_t calculateModemSourceChecksum(const ModemSourceRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ModemSourceRecord, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateModemSourceChecksum

// #region FUNC_calculateModemSourceV2Checksum
// PURPOSE: Checks legacy modem data before migration can consume it.
inline uint32_t calculateModemSourceV2Checksum(const ModemSourceRecordV2& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ModemSourceRecordV2, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

// #endregion FUNC_calculateModemSourceV2Checksum

// #region FUNC_isValidModemPollInterval
// PURPOSE: Centralizes the per-source poll interval contract (PLAN R6).
inline bool isValidModemPollInterval(uint16_t value) {
  return value >= kMinModemPollSec && value <= kMaxModemPollSec;
}
// #endregion FUNC_isValidModemPollInterval

// #region FUNC_isModemSourceRecordV2Valid
// PURPOSE: Rejects corrupt legacy modem profiles before migration.
inline bool isModemSourceRecordV2Valid(const ModemSourceRecordV2& record) {
  if (record.magic != kModemSourceMagic || record.version != 2 ||
      record.checksum != calculateModemSourceV2Checksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) return false;
  if (record.nitzTimeSyncEnabled != 0 && record.nitzTimeSyncEnabled != 1) return false;
  if (!isValidModemPollInterval(record.pollIntervalSec)) return false;
  return codec::isPrintableRange(record.label, sizeof(record.label));
}
// #endregion FUNC_isModemSourceRecordV2Valid

// #region FUNC_isModemSourceRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content
// and web input obey the same rules; the label is optional and may stay
// empty, the interval is bounded 5..300.
inline bool isModemSourceRecordValid(const ModemSourceRecord& record) {
  if (record.magic != kModemSourceMagic || record.version != kModemSourceVersion ||
      record.checksum != calculateModemSourceChecksum(record)) {
    return false;
  }
  if (record.moduleEnabled != 0 && record.moduleEnabled != 1) {
    return false;
  }
  if (record.pollEnabled != 0 && record.pollEnabled != 1) {
    return false;
  }
  if (record.nitzTimeSyncEnabled != 0 && record.nitzTimeSyncEnabled != 1) {
    return false;
  }
  if (record.smsPollEnabled != 0 && record.smsPollEnabled != 1) {
    return false;
  }
  if (!isValidModemPollInterval(record.pollIntervalSec)) {
    return false;
  }
  return codec::isPrintableRange(record.label, sizeof(record.label));
}
// #endregion FUNC_isModemSourceRecordValid
#endif  // MODEM_MODEM_RECORD_H
