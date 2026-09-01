// #region MODULE_CONTRACT
// PURPOSE: Proves bounded `millis()` deadlines preserve their timeout across rollover.
// SCOPE: Pure deadline comparisons before, at, and after a 32-bit rollover.
// INVARIANTS: Delays shorter than 2^31 ms do not expire before their deadline.
// #endregion MODULE_CONTRACT

#include <cassert>
#include <cstdint>
#include <cstdio>

#include "system/millis_deadline.h"

// #region FUNC_testDeadlineBeforeAndAfter
// PURPOSE: Pins ordinary deadline boundary semantics.
static void testDeadlineBeforeAndAfter() {
  constexpr uint32_t kDeadline = 2000;
  assert(!millis_deadline::reached(1999, kDeadline));
  assert(millis_deadline::reached(kDeadline, kDeadline));
  assert(millis_deadline::reached(2001, kDeadline));
}
// #endregion FUNC_testDeadlineBeforeAndAfter

// #region FUNC_testRolloverDeadline
// PURPOSE: Prevents a deadline that crosses rollover from expiring immediately.
static void testRolloverDeadline() {
  constexpr uint32_t kStartedAt = UINT32_MAX - 999;
  constexpr uint32_t kTimeoutMs = 2000;
  constexpr uint32_t kDeadline = kStartedAt + kTimeoutMs;
  assert(!millis_deadline::reached(kStartedAt, kDeadline));
  assert(!millis_deadline::reached(500, kDeadline));
  assert(millis_deadline::reached(kDeadline, kDeadline));
  assert(millis_deadline::reached(kDeadline + 1, kDeadline));

  constexpr uint32_t kZeroDeadlineStartedAt = UINT32_MAX - 999;
  constexpr uint32_t kZeroDeadline = kZeroDeadlineStartedAt + 1000U;
  static_assert(kZeroDeadline == 0, "deadline must wrap to zero");
  assert(!millis_deadline::reached(kZeroDeadlineStartedAt, kZeroDeadline));
  assert(millis_deadline::reached(kZeroDeadline, kZeroDeadline));
}
// #endregion FUNC_testRolloverDeadline

int main() {
  testDeadlineBeforeAndAfter();
  testRolloverDeadline();
  puts("all millis_deadline tests passed");
  return 0;
}
