// #region MODULE_CONTRACT
// PURPOSE: Proves task-stop timeouts preserve cancellation until the old task exits.
// SCOPE: stop-flag behavior for timeout, completed shutdown, and an already-stopped task.
// INVARIANTS: A live task handle retains an asserted stop flag after timeout.
// #endregion MODULE_CONTRACT

#include <cassert>
#include <cstdio>

#include "system/task_control.h"

namespace {
TaskHandle_t* handleToStop = nullptr;

// #region FUNC_finishTaskOnDelay
// PURPOSE: Emulates a task acknowledging cancellation during a stop poll slice.
void finishTaskOnDelay(unsigned long) { *handleToStop = nullptr; }
// #endregion FUNC_finishTaskOnDelay

// #region FUNC_testTimeoutRetainsStopRequest
// PURPOSE: Prevents a timed-out worker from resuming shared-resource work.
void testTimeoutRetainsStopRequest() {
  arduino_test::resetClock();
  TaskHandle_t handle = reinterpret_cast<TaskHandle_t>(1);
  volatile bool stopRequested = false;

  assert(!task_control::stopTask(handle, stopRequested, 25));
  assert(handle != nullptr);
  assert(stopRequested);
}
// #endregion FUNC_testTimeoutRetainsStopRequest

// #region FUNC_testCompletedStopClearsStopRequest
// PURPOSE: Prepares a completed task lifecycle for a replacement task.
void testCompletedStopClearsStopRequest() {
  arduino_test::resetClock();
  TaskHandle_t handle = reinterpret_cast<TaskHandle_t>(1);
  volatile bool stopRequested = false;
  handleToStop = &handle;
  arduino_test::delayHook = finishTaskOnDelay;

  assert(task_control::stopTask(handle, stopRequested, 25));
  assert(handle == nullptr);
  assert(!stopRequested);
}
// #endregion FUNC_testCompletedStopClearsStopRequest

// #region FUNC_testAlreadyStoppedTaskClearsStaleStopRequest
// PURPOSE: Lets a later lifecycle sync start a replacement after delayed exit.
void testAlreadyStoppedTaskClearsStaleStopRequest() {
  arduino_test::resetClock();
  TaskHandle_t handle = nullptr;
  volatile bool stopRequested = true;

  assert(task_control::stopTask(handle, stopRequested));
  assert(!stopRequested);
}
// #endregion FUNC_testAlreadyStoppedTaskClearsStaleStopRequest

}  // namespace

int main() {
  testTimeoutRetainsStopRequest();
  testCompletedStopClearsStopRequest();
  testAlreadyStoppedTaskClearsStaleStopRequest();
  puts("all task_control tests passed");
  return 0;
}
