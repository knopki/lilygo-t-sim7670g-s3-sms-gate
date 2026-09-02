// #region MODULE_CONTRACT
// PURPOSE: Keeps validation rules identical across persistence and HTTP forms.
// SCOPE:
//   - Validates printable credentials, constant-time equality, and bounded poll intervals.
//   - NOT: Persisting configuration records or handling HTTP requests.
// INVARIANTS:
//   - Accepted passwords remain within the shared printable-ASCII length limits.
//   - Secret comparisons inspect both inputs through their longest length.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_common.h"

#include "persistence/config_record.h"

// #region FUNC_isPrintableAscii
// PURPOSE: Rejects non-printable text before records and forms can diverge.
bool isPrintableAscii(const String& value) {
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < 32 || character > 126) {
      return false;
    }
  }
  return true;
}
// #endregion FUNC_isPrintableAscii

// #region FUNC_isValidPassword
// PURPOSE: Enforces one printable credential rule before values reach storage or auth.
bool isValidPassword(const String& value) {
  return value.length() >= kMinPasswordLength && value.length() <= kMaxPasswordLength &&
         isPrintableAscii(value);
}
// #endregion FUNC_isValidPassword

// #region FUNC_constantTimeEquals
// PURPOSE: Keeps secret comparisons from revealing length or content through timing.
bool constantTimeEquals(const String& left, const String& right) {
  const size_t longestLength = max(left.length(), right.length());
  uint8_t difference = static_cast<uint8_t>(left.length() ^ right.length());
  for (size_t index = 0; index < longestLength; ++index) {
    const char leftCharacter = index < left.length() ? left[index] : 0;
    const char rightCharacter = index < right.length() ? right[index] : 0;
    difference |= static_cast<uint8_t>(leftCharacter ^ rightCharacter);
  }
  return difference == 0;
}
// #endregion FUNC_constantTimeEquals

// #region FUNC_parsePollInterval
// PURPOSE: Rejects unsafe scheduler intervals before they reach persisted source profiles.
bool parsePollInterval(const String& raw, uint16_t& out, uint16_t min, uint16_t max,
                       String& error) {
  const String rangeError = String(F("Poll interval must be a number between ")) + String(min) +
                            F(" and ") + String(max) + F(" seconds.");
  String trimmed = raw;
  trimmed.trim();
  if (trimmed.length() == 0) {
    error = rangeError;
    return false;
  }
  char* end = nullptr;
  const long parsed = strtol(trimmed.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < static_cast<long>(min) ||
      parsed > static_cast<long>(max)) {
    error = rangeError;
    return false;
  }
  out = static_cast<uint16_t>(parsed);
  return true;
}
// #endregion FUNC_parsePollInterval
