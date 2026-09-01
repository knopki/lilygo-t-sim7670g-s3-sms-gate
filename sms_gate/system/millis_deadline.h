// #region MODULE_CONTRACT
// PURPOSE: Makes finite `millis()` deadlines correct across the 32-bit counter rollover.
// SCOPE:
// - Compares a current `millis()` value to a deadline produced by adding a bounded delay.
// - NOT: wall-clock time, scheduling, or delays of 2^31 ms or longer.
// INVARIANTS:
// - `reached()` remains correct when the deadline crosses the `millis()` rollover;
// - callers use delays shorter than 2^31 ms.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_MILLIS_DEADLINE_H
#define SYSTEM_MILLIS_DEADLINE_H

#include <cstdint>

namespace millis_deadline {

// #region FUNC_reached
// PURPOSE: Determines whether a bounded deadline has elapsed across rollover.
constexpr bool reached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<uint32_t>(nowMs - deadlineMs) < UINT32_C(0x80000000);
}
// #endregion FUNC_reached

}  // namespace millis_deadline
#endif  // SYSTEM_MILLIS_DEADLINE_H
