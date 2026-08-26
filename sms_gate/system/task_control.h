// #region MODULE_CONTRACT
// PURPOSE: Provides the single task-stop helper and the portMUX-backed
// status caches shared by the ZTE and modem poll lifecycles so
// syncZtePollTask/syncModemTask and publish*/read* stop diverging.
// SCOPE:
// - stopTask(handle, flag, timeout) with kTaskStopPollMs slices and
// StatusCache for trivially copyable snapshots.
// - NOT: HTTP routing, NVS persistence, and SMTP/ZTE/modem dialogs.
// INVARIANTS: The stop flag is always cleared on return; the cache never
// exposes its portMUX across String construction.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_TASK_CONTROL_H
#define SYSTEM_TASK_CONTROL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace task_control {

constexpr unsigned long kTaskStopTimeoutMs = 5000;
constexpr unsigned long kTaskStopPollMs = 10;
constexpr uint32_t kServiceTaskStack = 16384;
constexpr unsigned long kPollSliceMs = 250;

// #region FUNC_stopTask
// PURPOSE: Requests a task to stop via stopFlag and waits at most timeoutMs
// in kTaskStopPollMs slices until its handle clears.
// cppcheck-suppress constParameterReference
inline bool stopTask(TaskHandle_t& handle, volatile bool& stopFlag,
                     unsigned long timeoutMs = kTaskStopTimeoutMs) {
  if (handle == nullptr) {
    return true;
  }
  stopFlag = true;
  const unsigned long deadline = millis() + timeoutMs;
  while (handle != nullptr && millis() < deadline) {
    delay(kTaskStopPollMs);
  }
  stopFlag = false;
  return handle == nullptr;
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
