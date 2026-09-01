// #region MODULE_CONTRACT
// PURPOSE: Prevents concurrent modem tasks from corrupting shared Serial1 I/O.
// SCOPE:
// - lazy-created FreeRTOS mutex, take/give helpers.
// - NOT: AT parsing, NVS, HTTP.
// INVARIANTS:
// - Mutex is created once and never deleted;
// - every take is paired with a give even on error paths.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_MODEM_LOCK_H
#define SYSTEM_MODEM_LOCK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace modem_lock {

// #region FUNC_mutex
// PURPOSE: Gives every modem task the same lock so Serial1 access stays serialized.
inline SemaphoreHandle_t mutex() {
  static SemaphoreHandle_t handle = nullptr;
  if (handle == nullptr) {
    handle = xSemaphoreCreateMutex();
  }
  return handle;
}
// #endregion FUNC_mutex

// #region FUNC_take
// PURPOSE: Bounds contention so a modem task cannot wait indefinitely for Serial1.
inline bool take(unsigned long timeoutMs = 5000) {
  SemaphoreHandle_t m = mutex();
  if (m == nullptr) return false;
  return xSemaphoreTake(m, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
// #endregion FUNC_take

// #region FUNC_give
// PURPOSE: Restores access for the next modem operation after a guarded exchange.
inline void give() {
  SemaphoreHandle_t m = mutex();
  if (m != nullptr) xSemaphoreGive(m);
}
// #endregion FUNC_give

// #region CLASS_ScopedModemLock
// PURPOSE: Ensures a successful Serial1 lock acquisition is released exactly once.
class ScopedModemLock {
 public:
  explicit ScopedModemLock(unsigned long timeoutMs = 5000) : held_(take(timeoutMs)) {}
  ~ScopedModemLock() {
    if (held_) give();
  }

  ScopedModemLock(const ScopedModemLock&) = delete;
  ScopedModemLock& operator=(const ScopedModemLock&) = delete;

  bool held() const { return held_; }

 private:
  bool held_ = false;
};
// #endregion CLASS_ScopedModemLock

}  // namespace modem_lock

#endif  // SYSTEM_MODEM_LOCK_H
