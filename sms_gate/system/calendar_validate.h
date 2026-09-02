// #region MODULE_CONTRACT
// PURPOSE: Keeps calendar-date validation identical for modem time sources.
// SCOPE:
// - Validates Gregorian year, month, and day combinations.
// - NOT: Parsing wire formats or converting dates to epochs.
// INVARIANTS:
// - Leap days are accepted only in Gregorian leap years.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_CALENDAR_VALIDATE_H
#define SYSTEM_CALENDAR_VALIDATE_H

// #region FUNC_isValidCalendarDate
// PURPOSE: Rejects impossible Gregorian dates before timestamp conversion can normalize them.
inline bool isValidCalendarDate(int year, int month, int day) {
  if (month < 1 || month > 12) return false;
  const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int daysInMonth[] = {0, 31, leapYear ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return day >= 1 && day <= daysInMonth[month];
}
// #endregion FUNC_isValidCalendarDate

#endif  // SYSTEM_CALENDAR_VALIDATE_H
