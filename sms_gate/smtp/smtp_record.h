// #region MODULE_CONTRACT
// PURPOSE: Defines the portable, checksummed binary record for the SMTP
// delivery profile so it can be validated independently of Arduino hardware
// APIs.
// INVARIANTS: A record is valid only with the expected magic, version,
// checksum, and complete addresses. TLS trust comes from the firmware's
// embedded Mozilla root bundle, not from this record.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SMTP_SMTP_RECORD_H
#define SMTP_SMTP_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "codec.h"

constexpr size_t kMaxSmtpHostLength = 127;
constexpr size_t kMaxSmtpUserLength = 127;
constexpr size_t kMaxSmtpPasswordLength = 95;
constexpr size_t kMaxSmtpAddressLength = 127;
constexpr uint32_t kSmtpConfigMagic = 0x534D5450;  // "SMTP"
// Version history: 1 carried a pinned caCert field (not migrated; the single
// deployed operator re-enters settings); 2 = current layout after trust moved
// to the firmware's embedded root bundle (ADR-0002).
constexpr uint16_t kSmtpConfigVersion = 2;

// #region ENUM_SmtpSecurityMode
// PURPOSE: Selects how TLS is established: STARTTLS upgrade on port 587 or
// implicit TLS from the first byte on port 465.
enum class SmtpSecurityMode : uint8_t { kStartTls = 0, kImplicitTls = 1 };
// #endregion ENUM_SmtpSecurityMode

// #region CLASS_SmtpConfigRecord
// PURPOSE: Represents one complete SMTP delivery configuration as a single
// NVS blob, independent of the Wi-Fi record.
struct SmtpConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t port;
  uint8_t securityMode;
  char host[kMaxSmtpHostLength + 1];
  char username[kMaxSmtpUserLength + 1];
  char password[kMaxSmtpPasswordLength + 1];
  char fromAddress[kMaxSmtpAddressLength + 1];
  char recipientAddress[kMaxSmtpAddressLength + 1];
  uint32_t checksum;
};
// #endregion CLASS_SmtpConfigRecord

// #region FUNC_calculateSmtpConfigChecksum
// PURPOSE: Detects incomplete and incompatible records before their
// credentials can be used by the device.
inline uint32_t calculateSmtpConfigChecksum(const SmtpConfigRecord& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(SmtpConfigRecord, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}
// #endregion FUNC_calculateSmtpConfigChecksum

// #region FUNC_isSmtpConfigRecordValid
// PURPOSE: Gates loading and saving on one shared predicate so NVS content and
// web input obey the same rules.
inline bool isSmtpConfigRecordValid(const SmtpConfigRecord& record) {
  if (record.magic != kSmtpConfigMagic || record.version != kSmtpConfigVersion ||
      record.checksum != calculateSmtpConfigChecksum(record)) {
    return false;
  }
  if (record.port == 0 ||
      (record.securityMode != static_cast<uint8_t>(SmtpSecurityMode::kStartTls) &&
       record.securityMode != static_cast<uint8_t>(SmtpSecurityMode::kImplicitTls))) {
    return false;
  }
  if (!codec::isPrintableRange(record.host, kMaxSmtpHostLength) || record.host[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.username, kMaxSmtpUserLength) || record.username[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.password, kMaxSmtpPasswordLength) ||
      record.password[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.fromAddress, kMaxSmtpAddressLength) ||
      !codec::containsCharacter(record.fromAddress, kMaxSmtpAddressLength, '@') ||
      record.fromAddress[0] == '\0') {
    return false;
  }
  if (!codec::isPrintableRange(record.recipientAddress, kMaxSmtpAddressLength) ||
      !codec::containsCharacter(record.recipientAddress, kMaxSmtpAddressLength, '@') ||
      record.recipientAddress[0] == '\0') {
    return false;
  }
  return true;
}
// #endregion FUNC_isSmtpConfigRecordValid
#endif  // SMTP_SMTP_RECORD_H
