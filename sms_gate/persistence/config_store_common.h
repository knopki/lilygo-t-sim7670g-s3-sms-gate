// #region MODULE_CONTRACT
// PURPOSE: Keeps validation rules identical across persistence and HTTP forms.
// SCOPE:
// - isPrintableAscii, isValidPassword, constantTimeEquals, parsePollInterval.
// - NOT: NVS partition access and record checksums.
// INVARIANTS:
// - All callers use the same validation and bounded poll-interval parsing rules.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_COMMON_H
#define PERSISTENCE_CONFIG_STORE_COMMON_H

#include <Arduino.h>

// Shared NVS partition constants so every store uses one appcfg name/key (P1 dedup).
constexpr char kAppCfgPartition[] = "appcfg";
constexpr char kAppCfgKey[] = "record";

// #region FUNC_isPrintableAscii
// PURPOSE: Rejects configuration text that cannot safely cross modem/UI boundaries.
bool isPrintableAscii(const String& value);
// #endregion FUNC_isPrintableAscii

// #region FUNC_isValidPassword
// PURPOSE: Enforces the shared administrator and modem password policy.
bool isValidPassword(const String& value);
// #endregion FUNC_isValidPassword

// #region FUNC_constantTimeEquals
// PURPOSE: Compares secrets without exposing length-dependent timing.
bool constantTimeEquals(const String& left, const String& right);
// #endregion FUNC_constantTimeEquals

// #region FUNC_parsePollInterval
// PURPOSE: Applies one bounded poll interval rule to all source forms.
bool parsePollInterval(const String& raw, uint16_t& out, uint16_t min, uint16_t max, String& error);
// #endregion FUNC_parsePollInterval
#endif  // PERSISTENCE_CONFIG_STORE_COMMON_H
