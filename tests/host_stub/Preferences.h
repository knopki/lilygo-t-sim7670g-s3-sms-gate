// #region MODULE_CONTRACT
// PURPOSE: Supplies deterministic in-memory NVS blobs for persistence host tests.
// SCOPE: Implements only the Preferences operations used by configuration stores.
// #endregion MODULE_CONTRACT

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class Preferences {
 public:
  bool begin(const char*, bool readOnly, const char*) {
    readOnly_ = readOnly;
    return true;
  }

  void end() {}

  size_t getBytes(const char*, void* value, size_t maxLength) const {
    if (value == nullptr || maxLength < bytes_.size()) return 0;
    if (!bytes_.empty()) std::memcpy(value, bytes_.data(), bytes_.size());
    return bytes_.size();
  }

  size_t putBytes(const char*, const void* value, size_t length) {
    if (readOnly_ || value == nullptr) return 0;
    const auto* bytes = static_cast<const uint8_t*>(value);
    bytes_.assign(bytes, bytes + length);
    return length;
  }

  static void setBytes(const void* value, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    bytes_.assign(bytes, bytes + length);
  }

  static const std::vector<uint8_t>& bytes() { return bytes_; }

 private:
  bool readOnly_ = true;
  inline static std::vector<uint8_t> bytes_;
};
