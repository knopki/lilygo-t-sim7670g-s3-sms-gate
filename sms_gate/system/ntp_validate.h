// #region MODULE_CONTRACT
// PURPOSE: Shared NTP-server form sanitizing so POST /api/setup and
// POST /api/ntp apply one rule for ADR-0005 ntpServer1/2 + ntpEnabled.
// SCOPE:
// - Trim, 0..kMaxNtpServerLength printable-ASCII validation, and the
//   enabled-but-empty defaults rule as one pure function with a result
//   code; an Arduino String wrapper maps codes to operator messages.
// - NOT: SNTP runtime, TimeSync arbitration, HTTP routing, persistence.
// INVARIANTS: Output buffers are always NUL-terminated; validation order
// matches the historical form path (length before printability); defaults
// are applied only when NTP is enabled and both trimmed servers are empty.
// DEPENDENCIES: Pure C++ (stddef); kMaxNtpServerLength from the portable
// config record; the String wrapper requires the Arduino core.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_NTP_VALIDATE_H
#define SYSTEM_NTP_VALIDATE_H

#include <stddef.h>

#include "persistence/config_record.h"

// #region ENUM_NtpSanitizeResult
// PURPOSE: Tells the HTTP layer which operator message to emit.
enum class NtpSanitizeResult {
  kOk,
  kTooLong,
  kNotPrintable,
};
// #endregion ENUM_NtpSanitizeResult

// #region FUNC_ntpValidateIsSpace
// PURPOSE: Mirrors Arduino String::trim whitespace so host and device trim
// identically.
inline bool ntpValidateIsSpace(char character) {
  return character == ' ' || character == '\t' || character == '\n' || character == '\v' ||
         character == '\f' || character == '\r';
}
// #endregion FUNC_ntpValidateIsSpace

// #region FUNC_ntpValidateTrimmedLength
// PURPOSE: Returns the length of raw with leading/trailing whitespace removed
// so over-length input is rejected before any copy.
inline size_t ntpValidateTrimmedLength(const char* raw) {
  if (raw == nullptr) {
    return 0;
  }
  size_t start = 0;
  while (raw[start] != '\0' && ntpValidateIsSpace(raw[start])) {
    ++start;
  }
  size_t end = start;
  while (raw[end] != '\0') {
    ++end;
  }
  while (end > start && ntpValidateIsSpace(raw[end - 1])) {
    --end;
  }
  return end - start;
}
// #endregion FUNC_ntpValidateTrimmedLength

// #region FUNC_ntpValidateCopyTrimmed
// PURPOSE: Copies raw into out (at least kMaxNtpServerLength + 1 bytes)
// with leading/trailing whitespace removed; raw must already pass the
// length rule.
inline void ntpValidateCopyTrimmed(const char* raw, char* out) {
  if (raw == nullptr) {
    out[0] = '\0';
    return;
  }
  size_t start = 0;
  while (raw[start] != '\0' && ntpValidateIsSpace(raw[start])) {
    ++start;
  }
  size_t end = start;
  while (raw[end] != '\0') {
    ++end;
  }
  while (end > start && ntpValidateIsSpace(raw[end - 1])) {
    --end;
  }
  for (size_t index = 0; index < end - start; ++index) {
    out[index] = raw[start + index];
  }
  out[end - start] = '\0';
}
// #endregion FUNC_ntpValidateCopyTrimmed

// #region FUNC_sanitizeNtpServers
// PURPOSE: Validates and normalizes both NTP servers: trims, rejects
// non-printable ASCII, rejects over-length values, and fills the defaults
// when NTP is enabled with both servers empty. out1/out2 must provide
// kMaxNtpServerLength + 1 bytes each.
inline NtpSanitizeResult sanitizeNtpServers(bool enabled, const char* defaultServer1,
                                            const char* defaultServer2, const char* rawServer1,
                                            const char* rawServer2, char* outServer1,
                                            char* outServer2) {
  if (ntpValidateTrimmedLength(rawServer1) > kMaxNtpServerLength ||
      ntpValidateTrimmedLength(rawServer2) > kMaxNtpServerLength) {
    return NtpSanitizeResult::kTooLong;
  }
  ntpValidateCopyTrimmed(rawServer1, outServer1);
  ntpValidateCopyTrimmed(rawServer2, outServer2);
  size_t length1 = 0;
  size_t length2 = 0;
  while (outServer1[length1] != '\0') {
    ++length1;
  }
  while (outServer2[length2] != '\0') {
    ++length2;
  }
  for (size_t index = 0; index < length1; ++index) {
    if (outServer1[index] < 32 || outServer1[index] > 126) {
      return NtpSanitizeResult::kNotPrintable;
    }
  }
  for (size_t index = 0; index < length2; ++index) {
    if (outServer2[index] < 32 || outServer2[index] > 126) {
      return NtpSanitizeResult::kNotPrintable;
    }
  }
  if (enabled && length1 == 0 && length2 == 0) {
    ntpValidateCopyTrimmed(defaultServer1, outServer1);
    ntpValidateCopyTrimmed(defaultServer2, outServer2);
  }
  return NtpSanitizeResult::kOk;
}
// #endregion FUNC_sanitizeNtpServers

#ifdef ARDUINO
#include <WString.h>

#include "persistence/config_store_network.h"

// #region FUNC_sanitizeNtpFormStrings
// PURPOSE: Arduino String adapter for sanitizeNtpServers that applies the
// configured defaults and maps result codes to operator messages.
inline bool sanitizeNtpFormStrings(bool enabled, const String& rawServer1, const String& rawServer2,
                                   String& outServer1, String& outServer2, String& error) {
  char buffer1[kMaxNtpServerLength + 1];
  char buffer2[kMaxNtpServerLength + 1];
  const NtpSanitizeResult result =
      sanitizeNtpServers(enabled, kDefaultNtpServer1, kDefaultNtpServer2, rawServer1.c_str(),
                         rawServer2.c_str(), buffer1, buffer2);
  if (result == NtpSanitizeResult::kTooLong) {
    error = F("NTP server must contain up to 64 printable characters.");
    return false;
  }
  if (result == NtpSanitizeResult::kNotPrintable) {
    error = F("NTP server must contain printable ASCII.");
    return false;
  }
  outServer1 = buffer1;
  outServer2 = buffer2;
  return true;
}
// #endregion FUNC_sanitizeNtpFormStrings
#endif
#endif  // SYSTEM_NTP_VALIDATE_H
