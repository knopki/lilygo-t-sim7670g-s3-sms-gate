# ADR-0006: Watchdog supervisor (TWDT 60s + boot-loop safe-mode)

- **Status:** Accepted
- **Date:** 2026-08-26
- **Decides:** watchdog supervisor architecture
- **Context:** see below
- **Depends:** ADR-0004 (SIM7670G), task_control (FreeRTOS tasks)

## Context

Устройство работает автономно (LilyGO T-SIM7670G-S3, ESP32-S3). До этого WDT не было — любой зависший `loop()` или `modem_poll`/`gps_poll`/`zte_poll` оставлял шлюз немым до питания. При этом нормальная работа включает длительные блокировки: `WifiManager::testStationCandidate` 30 с, `modem_lock::take(12-15с)`, SMTP TLS 20–30 с, `zte_send` до 20 с. `loop()` обслуживает `WebServer::handleClient()` — зависание HTTP-хендлера тоже должно рестартовать.

Требования (уточнены):
1. Рестарт **только по зависанию**, не по `STA offline` / отсутствию SIM / SMTP.
2. TWDT таймаут **60 с** ( > 30 с worst-case + запас).
3. Перед `esp_restart()` дёрнуть **модем RESET** (GPIO17 LOW 200 мс) — залипший SIM7670G не переживает рестарт ESP.
4. После **3 watchdog-рестартов без стабильного окна 5 мин** уйти в **safe-mode**: не стартовать `modem_poll`/`gps_poll`/`zte_poll`, оставить AP+HTTP для диагностики. Сброс safe-mode — `POST /api/watchdog/clear` или 5 мин аптайма.

Опции:
- **Только `enableLoopWDT` из Arduino menuconfig.** Просто, но не покрывает poll-задачи и не даёт safe-mode / модем-RESET.
- **Внешний WDT чип.** Надёжнее, но на плате его нет — YAGNI.
- **TWDT + RTC-супервизор (выбран).** Использует встроенные `MWDT0/RWDT/TWDT` (panic → `ESP_RST_TASK_WDT`), RTC_NOINIT счётчик, единый `triggerRestart()`. Покрывает loop + все poll-задачи, без NVS-миграции.

## Decision

Вводится модуль `system/watchdog.*` — единственный владелец `esp_task_wdt_*` и `esp_restart()`:

- **TWDT:** `timeout_ms=60000, idle_core_mask=0, trigger_panic=true`. `watchdog::begin()` инициализирует/реконфигурирует и делает `add(loopTask)`. Каждая poll-задача: `addCurrentTask(name)` при старте, `reset()` каждый `kPollSliceMs=250 мс` в idle-ожиданиях, `removeCurrentTask()` перед `vTaskDelete`.
- **Кормление loop:** `watchdog::feedLoop()` в начале `loopFirmware()` → `esp_task_wdt_reset()` + обновление `lastFeedLoopMs`.
- **Супервизор:** `watchdog::loop()` в каждом `loopFirmware()`: (a) если `now - lastFeedLoopMs > 180 с` → `triggerRestart("loop_stall")` (fallback если TWDT не сработал), (b) если `millis() > 5 мин` → сброс RTC `bootCount/safeMode`.
- **Boot-loop:** `RTC_NOINIT_ATTR RtcState {magic, bootCount, lastWasWatchdog, safeMode}`. `begin()` инкрементирует `bootCount` только если `lastWasWatchdog==1` или `reset_reason ∈ {TASK_WDT,WDT,INT_WDT,PANIC,SW+mark}`; `POWERON/BROWNOUT/EXT` сбрасывает. При `bootCount ≥3` → `safeMode=true`, `sms_gate.ino` пропускает `syncPollTask()/syncTask()`. Перед каждым `esp_restart()` ставим `lastWasWatchdog=1`, делаем `resetModemHardware()` (RESET LOW 200 мс + DTR LOW).
- **Наблюдаемость:** `event=watchdog_init/boot/boot_count/safe_mode/stall_trigger/trigger/stable_clear`, `GET /api/watchdog {safe_mode,boot_count,timeout_sec,last_reset_reason,uptime_ms}`, `POST /api/watchdog/clear` (Digest). `bootTrace` уже логирует `reset_reason`.
- **Исключения чтобы не ложно сработать:** `WifiManager::testStationCandidate` теперь кормит `watchdog::feedLoop()` каждую итерацию `delay(100)`; SMTP/modem/ZTE задачи кормят WDT в каждом `vTaskDelay` слайсе.

`sms_gate.ino` вызывает `watchdog::begin()` первым после `Serial.begin()`, `feedLoop()+loop()` в начале `loopFirmware()`.

## Alternatives Considered

### Only Arduino loop WDT

Нет покрытия poll-задач, нет модем-RESET, нет safe-mode. Отклонён: не решает зависание `modem_lock`.

### External HW watchdog

Требует плату и GPIO — отсутствует, YAGNI.

### Full task health monitoring (heap, Wi-Fi RSSI, SMTP queue)

Дало бы больше сигналов, но увеличивает ложные рестарты и код. Отложено: покрывается TWDT + существующими `event=*_error`.

## Consequences

- **Positive:** зависший loop/poll перезапускает ≤60 с (TWDT panic) или ≤180 с (супервизор); модем ресетится перед ESP; флэппинг (3 × watchdog без 5 мин стабильности) не уходит в boot-loop — остаётся AP+HTTP для `clear`.
- **Negative / trade-offs:** RTC_NOINIT теряется при полном обесточивании — тогда счётчик сбрасывается (приемлемо, т.к. питание уже сбросило модем). TWDT panic пишет coredump в существующий партишн `coredump 0xFF0000 0x10000` — читать `esptool read_flash`. Добавляет ~2 КБ flash.
- **Accepted risks:** TWDT panic = hard fault, `Serial.flush()` может не успеть — логируем до RESET узким `printf`. Компиляция требует `esp_task_wdt.h` из Arduino-ESP32 3.x (API `esp_task_wdt_config_t`).
