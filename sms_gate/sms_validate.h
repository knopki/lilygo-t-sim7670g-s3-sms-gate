// #region MODULE_CONTRACT
// PURPOSE: Shared SMS-send validation so the ZTE MF79RU and SIM7670G paths
// use one recipient rule and one 335-unit limit; the future unified
// POST /api/sms/send {via:"zte"|"modem"} needs no storage change.
// SCOPE:
// - Recipient validation (3-20 digits with optional leading +), UTF-16 unit
// counting for UCS-2/UNICODE payloads, and the 335-unit modem UI limit
// shared by both modems.
// - NOT: Transport, AT/goform dialogs, NVS, HTTP routing, and GNSS.
// INVARIANTS: Malformed UTF-8 is never counted as valid text; the same
// limit gates the web form and the send path so an accepted form always
// fits the modem request; no credentials are inspected.
// DEPENDENCIES: Pure C++ (stddef, stdint, string); Arduino String overload
// is guarded so host tests compile without Arduino headers.
// #endregion MODULE_CONTRACT

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <WString.h>
#endif

// #region STRUCT_SmsValidateConstants
// PURPOSE: One limit for every SMS source, matching the modem web UI's
// UNICODE send limit (five concatenated UCS-2 parts).
constexpr size_t kMaxSmsSendUnits = 335;
constexpr size_t kSmsInvalidUnits = SIZE_MAX;
constexpr size_t kMinSmsRecipientLength = 3;
constexpr size_t kMaxSmsRecipientLength = 20;
// #endregion STRUCT_SmsValidateConstants

// #region FUNC_smsValidateDecodeUtf8One
// PURPOSE: Decodes one UTF-8 codepoint for unit counting, rejecting overlong
// forms, surrogates, and truncated sequences.
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
