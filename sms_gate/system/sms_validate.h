// #region MODULE_CONTRACT
// PURPOSE: Keeps recipient and message limits identical across SMS paths.
// SCOPE:
// - Validates recipients and counts UTF-16 units within the shared send
// limit.
// - NOT: transports, modem dialogs, NVS, HTTP routes, or GNSS.
// INVARIANTS:
// - Malformed UTF-8 is rejected;
// - accepted text fits both send paths;
// - no credentials are inspected.
// DEPENDENCIES: Pure C++; optional Arduino String overload.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_SMS_VALIDATE_H
#define SYSTEM_SMS_VALIDATE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <WString.h>
#endif

// #region CONST_SmsValidateConstants
// PURPOSE: One limit for every SMS source, matching the modem web UI's
// UNICODE send limit (five concatenated UCS-2 parts).
constexpr size_t kMaxSmsSendUnits = 335;
constexpr size_t kSmsInvalidUnits = SIZE_MAX;
constexpr size_t kMinSmsRecipientLength = 3;
constexpr size_t kMaxSmsRecipientLength = 20;
// #endregion CONST_SmsValidateConstants

// #region FUNC_smsValidateDecodeUtf8One
// PURPOSE: Decodes one UTF-8 codepoint for unit counting, rejecting overlong
// forms, surrogates, and truncated sequences.
// REQUIRES: p points into a readable NUL-terminated byte string; validation
// may inspect up to three bytes after the leading byte.
inline bool smsValidateDecodeUtf8One(const char*& p, uint32_t& out) {
  const unsigned char lead = static_cast<unsigned char>(*p++);
  if (lead < 0x80) {
    out = lead;
    return true;
  }
  size_t cont = 0;
  uint32_t min = 0;
  if ((lead & 0xE0) == 0xC0) {
    cont = 1;
    min = 0x80;
    out = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    cont = 2;
    min = 0x800;
    out = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    cont = 3;
    min = 0x10000;
    out = lead & 0x07;
  } else {
    return false;
  }
  for (size_t i = 0; i < cont; ++i) {
    const unsigned char b = static_cast<unsigned char>(p[i]);
    if ((b & 0xC0) != 0x80) {
      return false;
    }
    out = (out << 6) | (b & 0x3F);
  }
  p += cont;
  if (out < min || out > 0x10FFFF || (out >= 0xD800 && out <= 0xDFFF)) {
    return false;
  }
  return true;
}
// #endregion FUNC_smsValidateDecodeUtf8One

// #region FUNC_smsUtf16Units
// PURPOSE: Counts the UTF-16 code units a UTF-8 text occupies once encoded
// as UCS-2 hex; returns kSmsInvalidUnits on malformed UTF-8 so callers
// share one length rule between the web form and send paths.
inline size_t smsUtf16Units(const char* utf8) {
  if (utf8 == nullptr) {
    return kSmsInvalidUnits;
  }
  size_t units = 0;
  const char* p = utf8;
  while (*p != '\0') {
    uint32_t cp = 0;
    if (!smsValidateDecodeUtf8One(p, cp)) {
      return kSmsInvalidUnits;
    }
    units += cp > 0xFFFF ? 2 : 1;
  }
  return units;
}
// #endregion FUNC_smsUtf16Units

// #region FUNC_isValidSmsRecipient
// PURPOSE: Accepts only an optional leading plus followed by digits so the
// Number field can never smuggle form separators into a modem request.
inline bool isValidSmsRecipient(const char* value) {
  if (value == nullptr) {
    return false;
  }
  const size_t len = strlen(value);
  if (len < kMinSmsRecipientLength || len > kMaxSmsRecipientLength) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char ch = value[i];
    if (ch == '+' && i == 0) {
      continue;
    }
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}
#ifdef ARDUINO
inline bool isValidSmsRecipient(const String& value) { return isValidSmsRecipient(value.c_str()); }
#endif
// #endregion FUNC_isValidSmsRecipient
#endif  // SYSTEM_SMS_VALIDATE_H
