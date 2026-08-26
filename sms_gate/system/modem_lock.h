// #region MODULE_CONTRACT
// PURPOSE: Provides one global mutex that serialises access to the shared
// SIM7670G Serial1 channel between the modem SMS and GNSS poll tasks.
// SCOPE: lazy-created FreeRTOS mutex, take/give helpers.
// NOT: AT parsing, NVS, HTTP.
// INVARIANTS: Mutex is created once and never deleted; every take is paired
// with a give even on error paths.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_MODEM_LOCK_H
#define SYSTEM_MODEM_LOCK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace modem_lock {

// #region FUNC_mutex
// PURPOSE: Returns the singleton modem channel mutex, creating it on first use.
inline SemaphoreHandle_t mutex() {
  static SemaphoreHandle_t handle = nullptr;
  if (handle == nullptr) {
    handle = xSemaphoreCreateMutex();
  }
  return handle;
}
// #endregion FUNC_mutex

// #region FUNC_take
// PURPOSE: Blocks at most timeoutMs for the modem mutex.
inline bool take(unsigned long timeoutMs = 5000) {
  SemaphoreHandle_t m = mutex();
  if (m == nullptr) return false;
  return xSemaphoreTake(m, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
// #endregion FUNC_take

// #region FUNC_give
// PURPOSE: Releases the modem mutex.
inline void give() {
  SemaphoreHandle_t m = mutex();
  if (m != nullptr) xSemaphoreGive(m);
}
// #endregion FUNC_give

}  // namespace modem_lock

#endif  // SYSTEM_MODEM_LOCK_H
