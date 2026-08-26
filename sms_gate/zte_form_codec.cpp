// #region MODULE_CONTRACT
// PURPOSE: Implements the form codec for the ZTE goform channel, mirroring
// the modem web UI's own percent-encoding and the reference forwarder's
// proven SEND_SMS shape.
// #endregion MODULE_CONTRACT

#include "zte/zte_form_codec.h"

#include <string.h>

// #region FUNC_isUnreservedFormByte
// PURPOSE: Recognizes the characters application/x-www-form-urlencoded may
// carry literally; every other byte is percent-escaped.
bool isUnreservedFormByte(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
         ch == '-' || ch == '_' || ch == '.' || ch == '~';
}
// #endregion FUNC_isUnreservedFormByte

// #region FUNC_appendFormEscaped
// PURPOSE: Appends one form-value percent-escaping every non-unreserved
// byte, always terminating the buffer; returns false when the destination
// is full.
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
// PURPOSE: Appends one fixed fragment after an escaped value; returns false
// when the destination is full.
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
