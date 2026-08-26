// #region MODULE_CONTRACT
// PURPOSE: Defines the portable, checksummed binary record for the onboard
// SIM7670G SMS source (ADR-0004) so it can be validated independently of
// Arduino hardware APIs.
// INVARIANTS: A record is valid only with the expected magic, version,
// checksum, printable label (0..31 chars), and poll interval 5..300 seconds.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_RECORD_H
#define MODEM_MODEM_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxModemLabelLength = 31;
constexpr uint32_t kModemSourceMagic = 0x4D44534D;  // "MDSM"
constexpr uint16_t kModemSourceVersion = 1;
constexpr uint16_t kDefaultModemPollSec = 15;
constexpr uint16_t kMinModemPollSec = 5;
constexpr uint16_t kMaxModemPollSec = 300;

// #region STRUCT_ModemSourceRecord
// PURPOSE: Represents the internal SIM7670G SMS source profile (enable flag,
// poll interval, and phone number or alias shown in forwarded emails) as a
// single NVS blob, independent of the Wi-Fi, SMTP, and ZTE records.
struct ModemSourceRecord {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint16_t pollIntervalSec;
  char label[kMaxModemLabelLength + 1];
  uint32_t checksum;
};
// #endregion STRUCT_ModemSourceRecord

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

// #region FUNC_isValidModemPollInterval
// PURPOSE: Centralizes the per-source poll interval contract (PLAN R6).
inline bool isValidModemPollInterval(uint16_t value) {
  return value >= kMinModemPollSec && value <= kMaxModemPollSec;
}
// #endregion FUNC_isValidModemPollInterval

// #region FUNC_isModemSourceRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content
// and web input obey the same rules; the label is optional and may stay
// empty, the interval is bounded 5..300.
inline bool isModemSourceRecordValid(const ModemSourceRecord& record) {
  if (record.magic != kModemSourceMagic || record.version != kModemSourceVersion ||
      record.checksum != calculateModemSourceChecksum(record)) {
    return false;
  }
  if (record.enabled != 0 && record.enabled != 1) {
    return false;
  }
  if (!isValidModemPollInterval(record.pollIntervalSec)) {
    return false;
  }
  return codec::isPrintableRange(record.label, sizeof(record.label));
}
// #endregion FUNC_isModemSourceRecordValid
#endif  // MODEM_MODEM_RECORD_H
