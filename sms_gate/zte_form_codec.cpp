// #region MODULE_CONTRACT
// PURPOSE: Prevents ZTE form values from changing meaning on the wire.
// SCOPE:
// - Encodes ZTE form fields into bounded application/x-www-form-urlencoded buffers.
// - NOT: Issuing ZTE HTTP requests or validating source configuration.
// INVARIANTS:
// - Reserved bytes are percent-encoded rather than emitted as form delimiters.
// - Successful output is NUL-terminated and remains within caller capacity.
// #endregion MODULE_CONTRACT

#include "zte/zte_form_codec.h"

#include <string.h>

// #region FUNC_isUnreservedFormByte
// PURPOSE: Keeps form delimiters from bypassing the modem's field encoding.
bool isUnreservedFormByte(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
         ch == '-' || ch == '_' || ch == '.' || ch == '~';
}
// #endregion FUNC_isUnreservedFormByte

// #region FUNC_appendFormEscaped
// PURPOSE: Keeps modem form values bounded and unambiguous on the wire.
bool appendFormEscaped(const char* value, char* out, size_t outSize, size_t& used) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const char* p = value; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (isUnreservedFormByte(ch)) {
      if (used + 1 >= outSize) {
        return false;
      }
      out[used++] = static_cast<char>(ch);
    } else {
      if (used + 3 >= outSize) {
        return false;
      }
      out[used++] = '%';
      out[used++] = kHex[ch >> 4];
      out[used++] = kHex[ch & 0x0F];
    }
  }
  out[used] = '\0';
  return true;
}
// #endregion FUNC_appendFormEscaped

// #region FUNC_appendLiteral
// PURPOSE: Keeps assembled form requests terminated and within capacity.
bool appendLiteral(const char* literal, char* out, size_t outSize, size_t& used) {
  const size_t length = strlen(literal);
  if (used + length + 1 > outSize) {
    return false;
  }
  memcpy(out + used, literal, length);
  used += length;
  out[used] = '\0';
  return true;
}
// #endregion FUNC_appendLiteral
