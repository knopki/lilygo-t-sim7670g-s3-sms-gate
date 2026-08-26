// #region MODULE_CONTRACT
// PURPOSE: Provides the MD5 message digest (RFC 1321) as lowercase hex for
// the ZTE goform AD anti-CSRF token, without heap or Arduino dependencies.
// SCOPE:
// - One-shot Md5 class (reset/update/final) and md5Hex helper that formats
// the 16-byte digest as 32 hex chars plus terminator.
// - NOT: password hashing or any security use of MD5 (see INVARIANTS).
// INVARIANTS: MD5 is used only for the ZTE goform AD token; never for
// password hashing or other security purposes.
// DEPENDENCIES: Pure C++; assumes little-endian host (ESP32-S3 and the x86
// test host both qualify).
// #endregion MODULE_CONTRACT

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace codec {

// #region CLASS_Md5
// PURPOSE: Computes one MD5 digest (RFC 1321) over sequential updates so
// arbitrary-length inputs need no full-size buffer.
class Md5 {
 public:
  Md5() { reset(); }

  void reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bitCount_ = 0;
    bufferUsed_ = 0;
  }

  void update(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    bitCount_ += static_cast<uint64_t>(length) * 8;
    while (length > 0) {
      const size_t take = length < 64 - bufferUsed_ ? length : 64 - bufferUsed_;
      for (size_t index = 0; index < take; ++index) {
        buffer_[bufferUsed_ + index] = *bytes++;
      }
      bufferUsed_ += take;
      length -= take;
      if (bufferUsed_ == 64) {
        processBlock(buffer_);
        bufferUsed_ = 0;
      }
    }
  }

  // Writes the 16-byte digest; reset() is required before reuse.
  void final(uint8_t out[16]) {
    uint8_t padding[72];
    size_t paddingLength = (bufferUsed_ < 56 ? 56 : 120) - bufferUsed_;
    padding[0] = 0x80;
    for (size_t index = 1; index < paddingLength; ++index) {
      padding[index] = 0;
    }
    const uint64_t bits = bitCount_;
    for (size_t index = 0; index < 8; ++index) {
      padding[paddingLength + index] = static_cast<uint8_t>(bits >> (8 * index));
    }
    update(padding, paddingLength + 8);
    for (size_t index = 0; index < 4; ++index) {
      out[index * 4 + 0] = static_cast<uint8_t>(state_[index]);
      out[index * 4 + 1] = static_cast<uint8_t>(state_[index] >> 8);
      out[index * 4 + 2] = static_cast<uint8_t>(state_[index] >> 16);
      out[index * 4 + 3] = static_cast<uint8_t>(state_[index] >> 24);
    }
  }

 private:
  static uint32_t rotateLeft(uint32_t value, uint32_t shift) {
    return (value << shift) | (value >> (32 - shift));
  }

  void processBlock(const uint8_t block[64]) {
    constexpr uint32_t kShifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                      5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    constexpr size_t kWordOf[64] = {0, 1, 2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                    1, 6, 11, 0,  5,  10, 15, 4,  9,  14, 3,  8,  13, 2,  7,  12,
                                    5, 8, 11, 14, 1,  4,  7,  10, 13, 0,  3,  6,  9,  12, 15, 2,
                                    0, 7, 14, 5,  12, 3,  10, 1,  8,  15, 6,  13, 4,  11, 2,  9};
    uint32_t words[16];
    for (size_t index = 0; index < 16; ++index) {
      words[index] = static_cast<uint32_t>(block[index * 4]) |
                     (static_cast<uint32_t>(block[index * 4 + 1]) << 8) |
                     (static_cast<uint32_t>(block[index * 4 + 2]) << 16) |
                     (static_cast<uint32_t>(block[index * 4 + 3]) << 24);
    }
    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    for (size_t step = 0; step < 64; ++step) {
      uint32_t f;
      uint32_t g = kWordOf[step];
      if (step < 16) {
        f = (b & c) | (~b & d);
      } else if (step < 32) {
        f = (d & b) | (~d & c);
      } else if (step < 48) {
        f = b ^ c ^ d;
      } else {
        f = c ^ (b | ~d);
      }
      const uint32_t temp = d;
      d = c;
      c = b;
      b = b + rotateLeft(a + f + words[g] + kSineTable[step], kShifts[step]);
      a = temp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
  }

  static constexpr uint32_t kSineTable[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
      0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
      0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
      0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
      0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
      0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
      0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
      0xeb86d391};

  uint32_t state_[4];
  uint64_t bitCount_ = 0;
  uint8_t buffer_[64] = {};
  size_t bufferUsed_ = 0;
};
// #endregion CLASS_Md5

// #region FUNC_md5Hex
// PURPOSE: Computes the lowercase hex MD5 of one buffer into 33 writable
// bytes; used for the ZTE goform AD token (see ADR-0003).
inline void md5Hex(const char* data, size_t length, char out[33]) {
  Md5 md5;
  md5.update(data, length);
  uint8_t digest[16];
  md5.final(digest);
  static const char kHex[] = "0123456789abcdef";
  for (size_t index = 0; index < 16; ++index) {
    out[index * 2] = kHex[digest[index] >> 4];
    out[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  out[32] = '\0';
}
// #endregion FUNC_md5Hex

}  // namespace codec
