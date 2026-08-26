// #region MODULE_CONTRACT
// PURPOSE: Re-exports the split codec modules (base64, MD5, UCS-2) and keeps
// the small field-validation helpers so existing includes keep compiling
// while new code can include the narrow headers directly (ISP).
// SCOPE:
// - Re-export of codec/codec_base64.h, codec/codec_md5.h, codec/codec_ucs2.h.
// - Thin helpers isPrintableRange and containsCharacter for checksummed records.
// - NOT: decoding, transport, or NVS.
// INVARIANTS: Backward compatible; MD5 remains only for ZTE goform AD token.
// DEPENDENCIES: Pure C++; codec/codec_*.h are header-only.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef CODEC_H
#define CODEC_H

#include <stddef.h>

#include "codec/codec_base64.h"
#include "codec/codec_md5.h"
#include "codec/codec_ucs2.h"

namespace codec {

// #region FUNC_isPrintableRange
// PURPOSE: Shares the printable ASCII rule of the checksummed configuration
// records so binary garbage cannot survive validation.
inline bool isPrintableRange(const char* value, size_t maxLength) {
  for (size_t index = 0; index < maxLength; ++index) {
    const char character = value[index];
    if (character == '\0') {
      return true;
    }
    if (character < 32 || character > 126) {
      return false;
    }
  }
  return false;  // Not null-terminated within the field limit.
}
// #endregion FUNC_isPrintableRange

// #region FUNC_containsCharacter
// PURPOSE: Cheap sanity check for address-shaped fields without pulling in a
// full parser.
inline bool containsCharacter(const char* value, size_t maxLength, char expected) {
  for (size_t index = 0; index < maxLength && value[index] != '\0'; ++index) {
    if (value[index] == expected) {
      return true;
    }
  }
  return false;
}
// #endregion FUNC_containsCharacter

}  // namespace codec
#endif  // CODEC_H
