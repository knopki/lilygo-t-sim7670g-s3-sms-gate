// #region MODULE_CONTRACT
// PURPOSE: Keeps UTF-8/UCS-2 SMS conversion identical across modem paths.
// SCOPE:
// - Hex digit check, parseHex4, UTF-8 appender (appendUtf8), UCS-2 hex
// validation (isUcs2HexView), hex→UTF-8 decoding (decodeUcs2HexView) with
// surrogate-pair joining, and UTF-8→UCS-2 hex encoding (encodeUcs2Hex).
// - NOT: base64, MD5, field validation, or any transport/NVS logic.
// INVARIANTS: Pure C++; no heap allocation; invalid surrogates replaced with
// U+FFFD; decoding/encoding truncate at outSize.
// DEPENDENCIES: None beyond <stddef.h> / <stdint.h> / <string.h>.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef CODEC_CODEC_UCS2_H
#define CODEC_CODEC_UCS2_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace codec {

// #region FUNC_isHexDigit
// PURPOSE: Recognizes one hexadecimal digit of the UCS-2 content encoding.
inline bool isHexDigit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
// #endregion FUNC_isHexDigit

// #region FUNC_parseHex4
// PURPOSE: Reads exactly four hex digits; returns 0x200000 on malformed input.
inline uint32_t parseHex4(const char* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    const char ch = p[i];
    uint32_t digit = 0;
    if (ch >= '0' && ch <= '9')
      digit = static_cast<uint32_t>(ch - '0');
    else if (ch >= 'a' && ch <= 'f')
      digit = static_cast<uint32_t>(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F')
      digit = static_cast<uint32_t>(ch - 'A' + 10);
    else
      return 0x200000;
    value = (value << 4) | digit;
  }
  return value;
}
// #endregion FUNC_parseHex4

// #region FUNC_appendUtf8
// PURPOSE: Appends one codepoint as UTF-8, replacing invalid unpaired surrogates
// with U+FFFD and truncating when out is full.
inline void appendUtf8(uint32_t codepoint, char* out, size_t outSize, size_t& used) {
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) codepoint = 0xFFFD;
  unsigned char bytes[4];
  size_t count = 0;
  if (codepoint < 0x80) {
    bytes[0] = static_cast<unsigned char>(codepoint);
    count = 1;
  } else if (codepoint < 0x800) {
    bytes[0] = static_cast<unsigned char>(0xC0 | (codepoint >> 6));
    bytes[1] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 2;
  } else if (codepoint < 0x10000) {
    bytes[0] = static_cast<unsigned char>(0xE0 | (codepoint >> 12));
    bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[2] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 3;
  } else {
    bytes[0] = static_cast<unsigned char>(0xF0 | (codepoint >> 18));
    bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 12) & 0x3F));
    bytes[2] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[3] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 4;
  }
  for (size_t i = 0; i < count && used + 1 < outSize; ++i)
    out[used++] = static_cast<char>(bytes[i]);
}
// #endregion FUNC_appendUtf8

// #region FUNC_isUcs2HexView
// PURPOSE: Validates a whole buffer as 4-hex-per-codepoint UCS-2 before decoding.
inline bool isUcs2HexView(const char* str, size_t length) {
  if (str == nullptr) return false;
  if (length == 0) return true;
  if (length % 4 != 0) return false;
  for (size_t i = 0; i < length; ++i)
    if (!isHexDigit(str[i])) return false;
  return true;
}
inline bool isUcs2HexView(const char* str) {
  return str == nullptr ? false : isUcs2HexView(str, strlen(str));
}
// #endregion FUNC_isUcs2HexView

// #region FUNC_decodeUcs2HexView
// PURPOSE: Decodes validated UCS-2 hex into UTF-8 (surrogate pairs joined,
// unpaired surrogates replaced, truncated at outSize). Returns used bytes.
inline size_t decodeUcs2HexView(const char* hex, size_t hexLength, char* out, size_t outSize) {
  if (hex == nullptr || out == nullptr || outSize == 0) return 0;
  size_t used = 0;
  const char* p = hex;
  const char* end = hex + hexLength;
  while (p + 4 <= end) {
    uint32_t cp = parseHex4(p);
    p += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF && p + 4 <= end) {
      uint32_t low = parseHex4(p);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        p += 4;
      }
    }
    appendUtf8(cp, out, outSize, used);
  }
  if (used < outSize)
    out[used] = '\0';
  else
    out[outSize - 1] = '\0';
  return used;
}
inline size_t decodeUcs2HexView(const char* hex, char* out, size_t outSize) {
  return hex == nullptr ? 0 : decodeUcs2HexView(hex, strlen(hex), out, outSize);
}
// #endregion FUNC_decodeUcs2HexView

inline void appendUcs2HexUnitInternal(uint16_t unit, char* out, size_t outSize, size_t& used) {
  static const char kHex[] = "0123456789ABCDEF";
  for (int shift = 12; shift >= 0 && used + 1 < outSize; shift -= 4)
    out[used++] = kHex[(unit >> shift) & 0x0F];
}
inline bool decodeUtf8OneForEncode(const char*& p, uint32_t& out) {
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
  } else
    return false;
  for (size_t i = 0; i < cont; ++i) {
    const unsigned char b = static_cast<unsigned char>(p[i]);
    if ((b & 0xC0) != 0x80) return false;
    out = (out << 6) | (b & 0x3F);
  }
  p += cont;
  if (out < min || out > 0x10FFFF || (out >= 0xD800 && out <= 0xDFFF)) return false;
  return true;
}

// #region FUNC_encodeUcs2Hex
// PURPOSE: Encodes UTF-8 text as UCS-2 hex for modem SMS payloads.
inline size_t encodeUcs2Hex(const char* utf8, char* out, size_t outSize) {
  if (utf8 == nullptr || out == nullptr || outSize == 0) return 0;
  size_t used = 0;
  const char* p = utf8;
  while (*p != '\0' && used + 1 < outSize) {
    uint32_t cp = 0;
    if (!decodeUtf8OneForEncode(p, cp)) {
      out[used] = '\0';
      return used;
    }
    if (cp <= 0xFFFF)
      appendUcs2HexUnitInternal(static_cast<uint16_t>(cp), out, outSize, used);
    else {
      const uint32_t off = cp - 0x10000;
      appendUcs2HexUnitInternal(static_cast<uint16_t>(0xD800 + (off >> 10)), out, outSize, used);
      appendUcs2HexUnitInternal(static_cast<uint16_t>(0xDC00 + (off & 0x3FF)), out, outSize, used);
    }
  }
  out[used] = '\0';
  return used;
}
// #endregion FUNC_encodeUcs2Hex

}  // namespace codec
#endif  // CODEC_CODEC_UCS2_H
