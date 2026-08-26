// #region MODULE_CONTRACT
// PURPOSE: Provides RFC 4648 base64 encoding primitives for SMTP AUTH LOGIN
// and other dialogs so they can stream long inputs without a second full-size
// buffer.
// SCOPE:
// - Padded base64 encode: single 3-byte chunk (encodeBase64Chunk) and full
// buffer (encodeBase64) with terminator.
// - NOT: decoding, line wrapping (caller decides), or any transport.
// INVARIANTS: Pure C++; no heap allocation.
// DEPENDENCIES: None beyond <stddef.h> / <stdint.h>.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef CODEC_CODEC_BASE64_H
#define CODEC_CODEC_BASE64_H

#include <stddef.h>
#include <stdint.h>

namespace codec {

// #region FUNC_encodeBase64Chunk
// PURPOSE: Encodes up to three bytes into four base64 characters so callers
// can stream long inputs without a second full-size buffer.
inline void encodeBase64Chunk(const unsigned char* in, size_t remaining, char* out) {
  constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char b0 = in[0];
  const unsigned char b1 = remaining > 1 ? in[1] : 0;
  const unsigned char b2 = remaining > 2 ? in[2] : 0;
  out[0] = kAlphabet[b0 >> 2];
  out[1] = kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
  out[2] = remaining > 1 ? kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
  out[3] = remaining > 2 ? kAlphabet[b2 & 0x3F] : '=';
}
// #endregion FUNC_encodeBase64Chunk

// #region FUNC_encodeBase64
// PURPOSE: Encodes a complete buffer as padded base64; returns the encoded
// length, or 0 when the output buffer (including the terminator) is too
// small.
inline size_t encodeBase64(const char* input, size_t length, char* out, size_t outSize) {
  const size_t needed = ((length + 2) / 3) * 4 + 1;
  if (outSize < needed) {
    return 0;
  }
  size_t used = 0;
  while (length > 0) {
    const size_t chunk = length < 3 ? length : 3;
    encodeBase64Chunk(reinterpret_cast<const unsigned char*>(input), chunk, out + used);
    used += 4;
    input += chunk;
    length -= chunk;
  }
  out[used] = '\0';
  return used;
}
// #endregion FUNC_encodeBase64

}  // namespace codec
#endif  // CODEC_CODEC_BASE64_H
