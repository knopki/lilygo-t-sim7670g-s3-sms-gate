// #region MODULE_CONTRACT
// PURPOSE: Provides the shared printable-ASCII, password, constant-time and
// poll-interval helpers so config_store, zte and modem source validation
// share one rule for 5–300 s intervals and one credential predicate.
// SCOPE:
// - isPrintableAscii, isValidPassword, constantTimeEquals, parsePollInterval.
// - NOT: NVS partition access and record checksums.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

// Shared NVS partition constants so every store uses one appcfg name/key (P1 dedup).
constexpr char kAppCfgPartition[] = "appcfg";
constexpr char kAppCfgKey[] = "record";

bool isPrintableAscii(const String& value);
bool isValidPassword(const String& value);
bool constantTimeEquals(const String& left, const String& right);
bool parsePollInterval(const String& raw, uint16_t& out, uint16_t min, uint16_t max, String& error);
