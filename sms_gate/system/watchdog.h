// #region MODULE_CONTRACT
// PURPOSE: Owns the Task WDT (60s, panic) and the boot-loop supervisor so
// the main firmware only calls begin/feed/loop and the supervisor owns the
// single esp_restart() path (modem RESET pulse before reboot, safe-mode
// quorum after 3 watchdog reboots in a row).
// SCOPE:
// - TWDT init/add/reset (loop + poll tasks), RTC_NOINIT boot counter,
//   stable-window (5 min) clearing, modem hardware RESET pulse,
//   supervisor stall check (loop feed window), safe-mode flag.
// - NOT: Wi-Fi FSM, NVS persistence, SMTP/ZTE/modem AT dialogs, HTTP route
//   table — callers only feed/trigger.
// INVARIANTS: Only watchdog.* calls esp_task_wdt_* and esp_restart(); modem
// RESET is always pulsed before a watchdog restart; safe-mode disables poll
// tasks until cleared; no credentials are logged.
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
// PURPOSE: Initialises RTC boot counter, TWDT (60s, panic) and subscribes the
// loop task. Must be the first call in setupFirmware() after Serial.begin().
void begin();
// #endregion FUNC_begin

// #region FUNC_isSafeMode
// PURPOSE: True when ≥kWatchdogBootLoopThreshold watchdog reboots without a
// stable 5-min window — caller should keep poll tasks stopped and AP active.
bool isSafeMode();
// #endregion FUNC_isSafeMode

// #region FUNC_feedLoop
// PURPOSE: Resets TWDT on behalf of the loop task and updates supervisor
// timestamp. Call at the top of loopFirmware().
void feedLoop();
// #endregion FUNC_feedLoop

// #region FUNC_addRemoveCurrentTask
// PURPOSE: Subscribe/unsubscribe the caller's FreeRTOS task to TWDT.
// Call addCurrentTask() at the start of each pollTask, remove before delete.
void addCurrentTask(const char* name);
void removeCurrentTask();
// #endregion FUNC_addRemoveCurrentTask

// #region FUNC_reset
// PURPOSE: Resets TWDT on behalf of the calling poll task.
void reset();
// #endregion FUNC_reset

// #region FUNC_loop
// PURPOSE: Supervisor tick — checks loop stall and clears the stable window.
// Call every loopFirmware() after feedLoop().
void loop();
// #endregion FUNC_loop

// #region FUNC_triggerRestart
// PURPOSE: Single restart path: logs reason, pulses modem RESET, marks RTC
// and calls esp_restart(). Never returns.
void triggerRestart(const char* reason);
// #endregion FUNC_triggerRestart

// #region FUNC_clearSafeMode
// PURPOSE: Clears the RTC boot-loop counter and safe-mode flag. Call from
// HTTP handler or after operator intervention.
void clearSafeMode();
// #endregion FUNC_clearSafeMode

// #region FUNC_status
// PURPOSE: Returns boot-loop count for /api/status (0 when clean).
uint32_t bootLoopCount();
// #endregion FUNC_status

uint32_t lastResetReasonCode();

}  // namespace watchdog

#endif  // SYSTEM_WATCHDOG_H
