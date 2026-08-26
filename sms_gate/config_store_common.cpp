// #region MODULE_CONTRACT
// PURPOSE: Implements the shared credential and poll-interval helpers used by
// every configuration store and HTTP form validator.
// #endregion MODULE_CONTRACT

#include "persistence/config_store_common.h"

#include "persistence/config_record.h"

// #region FUNC_isPrintableAscii
// PURPOSE: Returns true when every character is in 32..126.
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
// PURPOSE: Returns true when length is 8..63 and isPrintableAscii holds.
bool isValidPassword(const String& value) {
  return value.length() >= kMinPasswordLength && value.length() <= kMaxPasswordLength &&
         isPrintableAscii(value);
}
// #endregion FUNC_isValidPassword

// #region FUNC_constantTimeEquals
// PURPOSE: Compares two strings in constant time over the longer length.
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
// PURPOSE: Parses poll_interval as decimal 5..300 (or caller-supplied
// min/max) into out; on failure sets error to the operator message.
bool parsePollInterval(const String& raw, uint16_t& out, uint16_t min, uint16_t max,
                       String& error) {
  String trimmed = raw;
  trimmed.trim();
  if (trimmed.length() == 0) {
    error = F("Poll interval must be a number between 5 and 300 seconds.");
    return false;
  }
  char* end = nullptr;
  const long parsed = strtol(trimmed.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < static_cast<long>(min) ||
      parsed > static_cast<long>(max)) {
    error = F("Poll interval must be a number between 5 and 300 seconds.");
    return false;
  }
  out = static_cast<uint16_t>(parsed);
  return true;
}
// #endregion FUNC_parsePollInterval
