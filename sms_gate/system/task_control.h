// #region MODULE_CONTRACT
// PURPOSE: Keeps task shutdown and status snapshots consistent across sources.
// SCOPE:
// - stopTask(handle, flag, timeout) with kTaskStopPollMs slices and
// StatusCache for trivially copyable snapshots.
// - NOT: HTTP routing, NVS persistence, and SMTP/ZTE/modem dialogs.
// INVARIANTS:
// - A timed-out task retains its asserted stop flag until it exits;
// - the cache never exposes its portMUX across String construction.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_TASK_CONTROL_H
#define SYSTEM_TASK_CONTROL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "system/millis_deadline.h"

namespace task_control {

constexpr unsigned long kTaskStopTimeoutMs = 5000;
constexpr unsigned long kTaskStopPollMs = 10;
constexpr uint32_t kServiceTaskStack = 16384;
constexpr unsigned long kPollSliceMs = 250;

// #region FUNC_stopTask
// PURPOSE: Bounds task shutdown without allowing an unresponsive task to resume.
// ENSURES: Retains the stop flag after a timeout while the task handle remains valid.
// cppcheck-suppress constParameterReference
inline bool stopTask(TaskHandle_t& handle, volatile bool& stopFlag,
                     unsigned long timeoutMs = kTaskStopTimeoutMs) {
  if (handle == nullptr) {
    stopFlag = false;
    return true;
  }
  stopFlag = true;
  const uint32_t deadline = millis() + timeoutMs;
  while (handle != nullptr && !millis_deadline::reached(millis(), deadline)) {
    delay(kTaskStopPollMs);
  }
  if (handle != nullptr) return false;
  stopFlag = false;
  return true;
}
// #endregion FUNC_stopTask

// #region CLASS_StatusCache
// PURPOSE: Holds one snapshot of a trivially copyable status value behind a
// portMUX so HTTP readers never block poll writers.
template <typename T>
class StatusCache {
 public:
  void publish(const T& status) {
    portENTER_CRITICAL(&mux_);
    data_ = status;
    portEXIT_CRITICAL(&mux_);
  }

  T read() const {
    portENTER_CRITICAL(&mux_);
    T snapshot = data_;
    portEXIT_CRITICAL(&mux_);
    return snapshot;
  }

 private:
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  T data_{};
};
// #endregion CLASS_StatusCache

// #region CLASS_StringStatusCache
// PURPOSE: Holds one operator-facing poll line (char[N]) behind the same
// portMUX without exposing the lock across String allocations.
template <size_t N>
class StringStatusCache {
 public:
  void publish(const char* status) {
    portENTER_CRITICAL(&mux_);
    strncpy(data_, status != nullptr ? status : "", N - 1);
    data_[N - 1] = '\0';
    portEXIT_CRITICAL(&mux_);
  }

  String readString() const {
    portENTER_CRITICAL(&mux_);
    String snapshot(data_);
    portEXIT_CRITICAL(&mux_);
    return snapshot;
  }

 private:
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  char data_[N] = "";
};
// #endregion CLASS_StringStatusCache

}  // namespace task_control
#endif  // SYSTEM_TASK_CONTROL_H
