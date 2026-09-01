// #region MODULE_CONTRACT
// PURPOSE: Keeps NTP form validation identical across setup and settings.
// SCOPE:
// - Trims and validates bounded server fields, applies enabled defaults,
// and maps results to form errors.
// - NOT: SNTP runtime, TimeSync, HTTP, or NVS.
// INVARIANTS:
// - Outputs are terminated;
// - length checks precede printability;
// - defaults apply only when enabled and both fields are empty.
// DEPENDENCIES: Pure C++, config record limits, Arduino String wrapper.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_NTP_VALIDATE_H
#define SYSTEM_NTP_VALIDATE_H

#include <stddef.h>

#include "persistence/config_record.h"

// #region ENUM_NtpSanitizeResult
// PURPOSE: Keeps validation failures mapped to stable operator feedback.
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
// PURPOSE: Rejects over-length input before trimming can hide the true field size.
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
// PURPOSE: Produces normalized server fields after the shared length gate has passed.
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
