// #region MODULE_CONTRACT
// PURPOSE: Implements the watchdog supervisor (ADR-0006) — TWDT + RTC boot
// loop quorum + modem RESET-before-restart — so the sketch stays thin.
// #endregion MODULE_CONTRACT

#include "system/watchdog.h"

#ifdef ARDUINO
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
// Host stubs — allow pure logic unit tests.
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_INVALID_STATE = 1;
inline int esp_task_wdt_init(const void*) { return ESP_OK; }
inline int esp_task_wdt_reconfigure(const void*) { return ESP_OK; }
inline int esp_task_wdt_add(void*) { return ESP_OK; }
inline int esp_task_wdt_delete(void*) { return ESP_OK; }
inline int esp_task_wdt_reset() { return ESP_OK; }
inline int esp_reset_reason() { return 0; }
inline void esp_restart() {}
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#endif

#include <Arduino.h>

namespace watchdog {
namespace {

// #region CONST_watchdogRtc
constexpr uint32_t kRtcMagic = 0x57445447UL;  // 'WDTG'
struct RtcState {
  uint32_t magic = 0;
  uint32_t bootCount = 0;
  uint32_t lastWasWatchdog = 0;
  uint32_t safeMode = 0;
};

// RTC_NOINIT_ATTR survives esp_restart() but not power-off — exactly the
// boot-loop window we need. No NVS version bump required.
RTC_NOINIT_ATTR RtcState rtcState;
// #endregion CONST_watchdogRtc

bool gSafeMode = false;
bool gTwdtInitialised = false;
unsigned long gLastFeedLoopMs = 0;
bool gLoopFedOnce = false;
uint32_t gLastResetReason = 0;

// #region FUNC_resetModemHardware
// PURPOSE: Pulses the Classic SIM7670G RESET pin before any watchdog reboot
// so a crashed modem does not survive the ESP restart (user request §3).
void resetModemHardware() {
#ifdef ARDUINO
  // Pins match modem/modem_transport.h Classic map.
  constexpr int kPinReset = 17;
  constexpr int kPinPwrKey = 18;
  constexpr int kPinDtr = 9;
  pinMode(kPinReset, OUTPUT);
  pinMode(kPinPwrKey, OUTPUT);
  pinMode(kPinDtr, OUTPUT);
  // Hold RESET LOW 200 ms — modem reset active low.
  digitalWrite(kPinReset, LOW);
  digitalWrite(kPinDtr, LOW);
  digitalWrite(kPinPwrKey, LOW);
  delay(200);
  digitalWrite(kPinReset, HIGH);
  delay(500);
  // Leave DTR LOW (awake) and PWRKEY LOW for normal boot.
#endif
}
// #endregion FUNC_resetModemHardware

}  // namespace

// #region FUNC_begin
void begin() {
#ifdef ARDUINO
  gLastResetReason = static_cast<uint32_t>(esp_reset_reason());
  Serial.printf("event=watchdog_boot reset_reason=%u rtc_magic=%08x boot_count=%u\n",
                static_cast<unsigned>(gLastResetReason), static_cast<unsigned>(rtcState.magic),
                static_cast<unsigned>(rtcState.bootCount));
#else
  gLastResetReason = 0;
#endif

  // Validate RTC magic.
  if (rtcState.magic != kRtcMagic) {
    rtcState.magic = kRtcMagic;
    rtcState.bootCount = 0;
    rtcState.lastWasWatchdog = 0;
    rtcState.safeMode = 0;
  }

#ifdef ARDUINO
  const bool wasWatchdogMark = rtcState.lastWasWatchdog == 1;
  const bool wasWatchdogReset =
      (gLastResetReason == ESP_RST_TASK_WDT) || (gLastResetReason == ESP_RST_WDT) ||
      (gLastResetReason == ESP_RST_PANIC) || (gLastResetReason == ESP_RST_INT_WDT) ||
      (gLastResetReason == ESP_RST_SW && wasWatchdogMark);
  if (wasWatchdogReset) {
    rtcState.bootCount += 1;
    Serial.printf("event=watchdog_boot_count inc boot_count=%u\n",
                  static_cast<unsigned>(rtcState.bootCount));
  } else if (gLastResetReason == ESP_RST_POWERON || gLastResetReason == ESP_RST_EXT ||
             gLastResetReason == ESP_RST_BROWNOUT) {
    rtcState.bootCount = 0;
    rtcState.safeMode = 0;
    Serial.println("event=watchdog_boot_count_reset reason=poweron");
  } else if (!wasWatchdogMark) {
    // Normal SW reset or deepsleep wake without watchdog mark — optionally keep
    // count only for watchdog series; on clean reboot clear if stable window
    // would have passed, otherwise keep to avoid hiding flapping. For MVP
    // clear on any non-watchdog SW reboot to avoid false safe-mode.
    if (gLastResetReason == ESP_RST_SW) {
      rtcState.bootCount = 0;
      rtcState.safeMode = 0;
    }
  }
  rtcState.lastWasWatchdog = 0;
  if (rtcState.bootCount >= kWatchdogBootLoopThreshold) {
    rtcState.safeMode = 1;
    gSafeMode = true;
    Serial.printf("event=watchdog_safe_mode_enter boot_count=%u threshold=%u\n",
                  static_cast<unsigned>(rtcState.bootCount),
                  static_cast<unsigned>(kWatchdogBootLoopThreshold));
  } else {
    gSafeMode = rtcState.safeMode == 1;
    if (gSafeMode) {
      Serial.printf("event=watchdog_safe_mode_still_active boot_count=%u\n",
                    static_cast<unsigned>(rtcState.bootCount));
    }
  }
#else
  gSafeMode = false;
#endif

#ifdef ARDUINO
  esp_task_wdt_config_t cfg{};
  cfg.timeout_ms = kWatchdogTimeoutSec * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err == ESP_OK) {
    gTwdtInitialised = true;
    Serial.printf("event=watchdog_init timeout=%u panic=true\n",
                  static_cast<unsigned>(kWatchdogTimeoutSec));
  } else if (err == ESP_ERR_INVALID_STATE) {
    // Already initialised (e.g. after OTA) — reconfigure to our timeout.
    esp_err_t rerr = esp_task_wdt_reconfigure(&cfg);
    gTwdtInitialised = (rerr == ESP_OK);
    Serial.printf("event=watchdog_reconfigure result=%d timeout=%u\n", (int)rerr,
                  static_cast<unsigned>(kWatchdogTimeoutSec));
  } else {
    Serial.printf("event=watchdog_init_failed err=%d\n", (int)err);
  }
  if (gTwdtInitialised) {
    esp_err_t aerr = esp_task_wdt_add(NULL);
    if (aerr == ESP_OK) {
      Serial.println("event=watchdog_task_added name=loop");
    } else {
      Serial.printf("event=watchdog_task_add_failed name=loop err=%d\n", (int)aerr);
    }
  }
#endif
  gLastFeedLoopMs = millis();
  gLoopFedOnce = false;
}
// #endregion FUNC_begin

// #region FUNC_isSafeMode
bool isSafeMode() { return gSafeMode; }
// #endregion FUNC_isSafeMode

// #region FUNC_feedLoop
void feedLoop() {
  gLastFeedLoopMs = millis();
  gLoopFedOnce = true;
#ifdef ARDUINO
  if (gTwdtInitialised) {
    esp_task_wdt_reset();
  }
#endif
}
// #endregion FUNC_feedLoop

// #region FUNC_addRemoveCurrentTask
void addCurrentTask(const char* name) {
#ifdef ARDUINO
  if (!gTwdtInitialised) return;
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err == ESP_OK) {
    Serial.printf("event=watchdog_task_added name=%s\n", name ? name : "unknown");
  } else {
    Serial.printf("event=watchdog_task_add_failed name=%s err=%d\n", name ? name : "unknown",
                  (int)err);
  }
#else
  (void)name;
#endif
}

void removeCurrentTask() {
#ifdef ARDUINO
  if (!gTwdtInitialised) return;
  esp_task_wdt_delete(NULL);
  Serial.println("event=watchdog_task_removed");
#endif
}
// #endregion FUNC_addRemoveCurrentTask

// #region FUNC_reset
void reset() {
#ifdef ARDUINO
  if (gTwdtInitialised) esp_task_wdt_reset();
#endif
}
// #endregion FUNC_reset

// #region FUNC_loop
void loop() {
  const unsigned long now = millis();
  if (!gLoopFedOnce) return;

  // Stall supervisor — 180 s without a loop feed → modem RESET + reboot.
  // TWDT (60 s) normally fires first; this is the fallback when TWDT is
  // mis-configured or a task feeds TWDT but loop is still stuck logically.
  if (now - gLastFeedLoopMs > kWatchdogSupervisorStallMs) {
    Serial.printf("event=watchdog_stall_trigger elapsed=%lu threshold=%u\n", now - gLastFeedLoopMs,
                  static_cast<unsigned>(kWatchdogSupervisorStallMs));
    triggerRestart("loop_stall");
    return;
  }

  // Stable window: after 5 min of uninterrupted loop, clear boot-loop counter.
  if (now > kWatchdogStableMs && rtcState.bootCount != 0) {
    Serial.printf("event=watchdog_stable_clear uptime=%lu boot_count=%u\n", now,
                  static_cast<unsigned>(rtcState.bootCount));
    rtcState.bootCount = 0;
    rtcState.safeMode = 0;
    gSafeMode = false;
  }
  // Also clear safe-mode after stable window if we entered it.
  if (gSafeMode && now > kWatchdogStableMs) {
    Serial.printf("event=watchdog_safe_mode_clear uptime=%lu\n", now);
    rtcState.bootCount = 0;
    rtcState.safeMode = 0;
    gSafeMode = false;
  }
}
// #endregion FUNC_loop

// #region FUNC_triggerRestart
void triggerRestart(const char* reason) {
  const char* r = reason ? reason : "unknown";
  Serial.printf("event=watchdog_trigger reason=%s safe_mode=%s boot_count=%u\n", r,
                gSafeMode ? "true" : "false", static_cast<unsigned>(rtcState.bootCount));
  Serial.flush();
#ifdef ARDUINO
  rtcState.lastWasWatchdog = 1;
  // Pulse modem hardware before ESP restart (user request §3).
  resetModemHardware();
  delay(100);
  esp_restart();
#else
  (void)r;
#endif
  // Should not return; spin if it does.
  while (true) {
#ifdef ARDUINO
    delay(1000);
#else
    break;
#endif
  }
}
// #endregion FUNC_triggerRestart

// #region FUNC_clearSafeMode
void clearSafeMode() {
  rtcState.bootCount = 0;
  rtcState.safeMode = 0;
  rtcState.lastWasWatchdog = 0;
  gSafeMode = false;
  Serial.println("event=watchdog_safe_mode_cleared action=manual");
}
// #endregion FUNC_clearSafeMode

// #region FUNC_status
uint32_t bootLoopCount() { return rtcState.bootCount; }
uint32_t lastResetReasonCode() { return gLastResetReason; }
// #endregion FUNC_status

}  // namespace watchdog
