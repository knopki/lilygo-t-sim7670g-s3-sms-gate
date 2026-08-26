// #region MODULE_CONTRACT
// PURPOSE: Defines the portable, checksummed binary record for the one saved
// network profile so it can be validated independently of Arduino hardware
// APIs.
// INVARIANTS: A record is valid only with the expected magic, version,
// and checksum.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_RECORD_H
#define PERSISTENCE_CONFIG_RECORD_H

#include <stddef.h>
#include <stdint.h>

constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 63;
constexpr uint32_t kConfigMagic = 0x534D5347;  // "SMSG"
constexpr uint16_t kConfigVersion = 1;

// #region CLASS_ConfigRecord
// PURPOSE: Represents one complete network and administrator configuration as
// a single NVS blob.
struct ConfigRecord {
  uint32_t magic;
  uint16_t version;
  char ssid[kMaxSsidLength + 1];
  char wifiPassword[kMaxPasswordLength + 1];
  char adminPassword[kMaxPasswordLength + 1];
  uint32_t checksum;
};
// #endregion CLASS_ConfigRecord

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
#endif  // PERSISTENCE_CONFIG_RECORD_H
