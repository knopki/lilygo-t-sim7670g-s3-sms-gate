// #region MODULE_CONTRACT
// PURPOSE: Prevents a failed Serial1 lock acquisition from issuing an invalid release.
// SCOPE: ScopedModemLock acquisition, timeout propagation, and release ownership.
// INVARIANTS: A failed acquisition never invokes xSemaphoreGive.
// #endregion MODULE_CONTRACT

#include <cassert>

#include "system/modem_lock.h"

// #region FUNC_testFailedAcquisitionDoesNotRelease
// PURPOSE: Verifies that an unavailable mutex cannot be released by a non-owner.
static void testFailedAcquisitionDoesNotRelease() {
  freertos_test::reset(false);
  {
    modem_lock::ScopedModemLock lock(15000);
    assert(!lock.held());
  }
  assert(freertos_test::takeCalls == 1);
  assert(freertos_test::lastTimeout == 15000);
  assert(freertos_test::giveCalls == 0);
}
// #endregion FUNC_testFailedAcquisitionDoesNotRelease

// #region FUNC_testSuccessfulAcquisitionReleasesOnce
// PURPOSE: Verifies that the lock owner still releases Serial1 after its session.
static void testSuccessfulAcquisitionReleasesOnce() {
  freertos_test::reset(true);
  {
    modem_lock::ScopedModemLock lock(12000);
    assert(lock.held());
    assert(freertos_test::giveCalls == 0);
  }
  assert(freertos_test::takeCalls == 1);
  assert(freertos_test::lastTimeout == 12000);
  assert(freertos_test::giveCalls == 1);
}
// #endregion FUNC_testSuccessfulAcquisitionReleasesOnce

int main() {
  testFailedAcquisitionDoesNotRelease();
  testSuccessfulAcquisitionReleasesOnce();
  return 0;
}
