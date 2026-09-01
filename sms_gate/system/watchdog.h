// #region MODULE_CONTRACT
// PURPOSE: Recovers stalled firmware and contains repeated boot failures.
// SCOPE:
// - Owns TWDT feeding, boot-loop safe mode, and the modem reset path.
// - NOT: Wi-Fi, persistence, protocol dialogs, or HTTP routes.
// INVARIANTS:
// - Restart pulses modem RESET;
// - safe mode stops poll tasks;
// - no credentials are logged.
// DEPENDENCIES: esp_task_wdt.h, esp_system.h, FreeRTOS tasks.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_WATCHDOG_H
#define SYSTEM_WATCHDOG_H

#include <Arduino.h>

namespace watchdog {

// Tunables — compile-time, justified in ADR-0006.
constexpr uint32_t kWatchdogTimeoutSec = 60;
constexpr uint32_t kWatchdogSupervisorStallMs = 180UL * 1000UL;
constexpr uint32_t kWatchdogStableMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kWatchdogBootLoopThreshold = 3;

// #region FUNC_begin
// PURPOSE: Establishes recovery supervision before firmware services start.
void begin();
// #endregion FUNC_begin

// #region FUNC_isSafeMode
// PURPOSE: Lets startup contain repeated watchdog failures in safe mode.
bool isSafeMode();
// #endregion FUNC_isSafeMode

// #region FUNC_feedLoop
// PURPOSE: Proves the main loop is alive to the recovery supervisor.
void feedLoop();
// #endregion FUNC_feedLoop

// #region FUNC_addCurrentTask
// PURPOSE: Extends stall recovery to a running poll task.
void addCurrentTask(const char* name);
// #endregion FUNC_addCurrentTask

// #region FUNC_removeCurrentTask
// PURPOSE: Removes deleted tasks from watchdog supervision cleanly.
void removeCurrentTask();
// #endregion FUNC_removeCurrentTask

// #region FUNC_reset
// PURPOSE: Proves a poll task is still making progress.
void reset();
// #endregion FUNC_reset

// #region FUNC_loop
// PURPOSE: Detects main-loop stalls and records stable recovery progress.
void loop();
// #endregion FUNC_loop

// #region FUNC_triggerRestart
// PURPOSE: Makes every firmware restart follow the hardware recovery path.
void triggerRestart(const char* reason);
// #endregion FUNC_triggerRestart

// #region FUNC_clearSafeMode
// PURPOSE: Lets operator intervention exit safe mode deliberately.
void clearSafeMode();
// #endregion FUNC_clearSafeMode

// #region FUNC_bootLoopCount
// PURPOSE: Exposes boot-loop history so the status API can show recovery state.
uint32_t bootLoopCount();
// #endregion FUNC_bootLoopCount

// #region FUNC_lastResetReasonCode
// PURPOSE: Lets operators identify the reset that triggered recovery.
uint32_t lastResetReasonCode();
// #endregion FUNC_lastResetReasonCode

}  // namespace watchdog

#endif  // SYSTEM_WATCHDOG_H
