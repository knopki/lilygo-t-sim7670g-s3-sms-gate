# ADR-0006: Watchdog supervisor (TWDT 60 s and boot-loop safe mode)

- **Status:** Accepted
- **Date:** 2026-08-26

## Context

The device works without operator control. The device is a LilyGO
T-SIM7670G-S3 with an ESP32-S3. The firmware had no watchdog. A stalled
`loop()`, `modem_poll`, `gps_poll`, or `zte_poll` left the gateway silent until
the operator removed power.

Normal operation has long blocking operations:

- `WifiManager::testStationCandidate` can block for 30 s.
- `modem_lock::take` can block for 12 to 15 s.
- SMTP TLS can block for 20 to 30 s.
- `zte_send` can block for up to 20 s.

The `loop()` function also calls `WebServer::handleClient()`. A stalled HTTP
handler must also cause a restart.

The requirements are:

1. Restart only when the firmware stalls. Do not restart because the station
   is offline, the SIM is absent, or SMTP fails.
2. Set the TWDT timeout to 60 s. This value is greater than the 30 s worst-case
   operation and provides additional time.
3. Set the modem RESET signal LOW for 200 ms before `esp_restart()`. A stalled
   SIM7670G can remain stalled after an ESP restart.
4. Enter safe mode after three watchdog restarts without a stable five-minute
   window. In safe mode, do not start `modem_poll`, `gps_poll`, or `zte_poll`.
   Keep the AP and HTTP server available for diagnosis. Exit safe mode with
   `POST /api/watchdog/clear` or after five minutes of uptime.

The following options were considered:

- **Use only `enableLoopWDT` from the Arduino menuconfig.** This option is
  simple, but it does not cover poll tasks and does not provide safe mode or a
  modem reset.
- **Use an external watchdog chip.** This option is more reliable, but the
  board has no such chip. It is not needed for this scope.
- **Use TWDT with an RTC supervisor (selected).** This option uses the built-in
  `MWDT0`, `RWDT`, and `TWDT` functions. A panic causes
  `ESP_RST_TASK_WDT`. An `RTC_NOINIT` counter and one `triggerRestart()`
  function provide the restart policy. The option covers `loop()` and all poll
  tasks without an NVS migration.

## Decision

Add `system/watchdog.*`. This module is the only owner of
`esp_task_wdt_*` and `esp_restart()`.

- **TWDT:** Use `timeout_ms=60000`, `idle_core_mask=0`, and
  `trigger_panic=true`. `watchdog::begin()` initializes or reconfigures the
  TWDT and adds `loopTask`. Each poll task calls `addCurrentTask(name)` when it
  starts. It calls `reset()` during each 250 ms idle wait and calls
  `removeCurrentTask()` before `vTaskDelete`.
- **Feed the loop task:** Call `watchdog::feedLoop()` at the start of
  `loopFirmware()`. The function calls `esp_task_wdt_reset()` and updates
  `lastFeedLoopMs`.
- **Supervisor:** Call `watchdog::loop()` during every `loopFirmware()` call.
  If `now - lastFeedLoopMs > 180 s`, call `triggerRestart("loop_stall")` as a
  fallback if TWDT does not act. If `millis() > 5 min`, clear the RTC
  `bootCount` and `safeMode` values.
- **Boot-loop protection:** Store
  `RTC_NOINIT_ATTR RtcState {magic, bootCount, lastWasWatchdog, safeMode}`.
  In `begin()`, increment `bootCount` only when `lastWasWatchdog == 1` or the
  reset reason is one of `TASK_WDT`, `WDT`, `INT_WDT`, `PANIC`, or
  `SW+mark`. Clear the count for `POWERON`, `BROWNOUT`, and `EXT` reset
  reasons. Set `safeMode=true` when `bootCount >= 3`.

  In safe mode, `sms_gate.ino` immediately starts the protected fallback AP
  and does not start the STA connection. `WifiManager` keeps this AP active.
  `syncPollTask()` and `syncTask()` also prevent poll task creation. This
  second check applies even when an HTTP save handler runs. `POST
  /api/watchdog/clear` removes the block, resumes STA connection, and restarts
  the poll services.

  Before each `esp_restart()`, set `lastWasWatchdog=1` and call
  `resetModemHardware()`. This sets RESET LOW for 200 ms and sets DTR LOW.
- **Observability:** Write the events `watchdog_init`, `watchdog_boot`,
  `watchdog_boot_count`, `watchdog_safe_mode`, `watchdog_stall_trigger`,
  `watchdog_trigger`, and `watchdog_stable_clear`. Provide
  `GET /api/watchdog {safe_mode,boot_count,timeout_sec,last_reset_reason,uptime_ms}`
  and Digest-authenticated `POST /api/watchdog/clear`. `bootTrace` already
  records the reset reason.
- **Prevent false resets:** `WifiManager::testStationCandidate` calls
  `watchdog::feedLoop()` during each `delay(100)` iteration. SMTP, modem, and
  ZTE tasks feed the watchdog during each `vTaskDelay` slice.

`sms_gate.ino` calls `watchdog::begin()` first after `Serial.begin()`. It calls
`feedLoop()` and `loop()` at the start of `loopFirmware()`.

## Alternatives Considered

### Only the Arduino loop WDT

This option does not cover poll tasks. It does not reset the modem and does
not provide safe mode. It does not prevent a `modem_lock` stall. Rejected.

### External hardware watchdog

This option requires a watchdog chip and a GPIO. The board has neither. It is
not needed for this scope. Rejected.

### Full task health monitoring

This option would monitor heap use, Wi-Fi RSSI, and the SMTP queue. It would
provide more signals, but it would increase code size and false restarts.
Existing `event=*_error` events cover the related errors. Deferred.

## Consequences

- **Positive:** A stalled loop or poll task restarts within 60 s through TWDT,
  or within 180 s through the supervisor. The modem resets before the ESP.
  Three watchdog resets without a stable five-minute window lead to AP and
  HTTP safe mode instead of an endless boot loop.
- **Negative / trade-offs:** `RTC_NOINIT` data is lost when power is removed.
  The counter then resets. This is acceptable because the power removal also
  resets the modem. TWDT panic writes a coredump to the existing
  `coredump 0xFF0000 0x10000` partition. Read it with `esptool read_flash`.
  The module adds about 2 KiB of flash use.
- **Accepted risks:** A TWDT panic is a hard fault. `Serial.flush()` might not
  complete. Log the event with a narrow `printf` before RESET. Compilation
  requires `esp_task_wdt.h` from Arduino-ESP32 3.x and the
  `esp_task_wdt_config_t` API.
