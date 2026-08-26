# ADR-0005: Unified time sync with source arbitration and NTP stratum 1

- **Status:** Accepted
- **Date:** 2026-08-26

## Context

The gateway has three independent wall-clock domains:

- **ESP32 system clock** (`time(nullptr)` / `gettimeofday`): unset after cold boot (no battery), set via `configTime()` SNTP when STA has internet, or via `gps_service settimeofday()` from GNSS (`+CGPSINFO` → ISO → epoch, deadband `|diff|>2s`).
- **SIM7670G modem clock** (`AT+CCLK?` → `ModemStatus.cclk`, format `yy/MM/dd,hh:mm:ss+zz`): unset (`""` / epoch-zero with no SIM), set by **NITZ** when `CEREG/CREG 1/5` (network time, `docs/research/modem-sim7670g.md:87`). Polled every `pollIntervalMs` but never feeds the system clock.
- **ZTE MF79RU clock**: its own SNTP, validates `sms_time` against it and rejects far-off timestamps; the gateway currently guards `ZteService::startSend` with `time(nullptr) < 2020-01-01` and sends a placeholder `00;01;01;00;00;00;+0` when unsynced.

Problems:

- `time() < 2020-01-01` is only a boolean "ever synced". For an **NTP server claiming stratum 1** we need `TimeState{source, lastSyncMs, accuracy, stratum}` — not just synced/unsynced.
- No arbitration: SNTP ( `±10-50 ms` ) is more accurate than NITZ ( `±1-2 s`, second granularity, operator-dependent `+zz` ), which is far less accurate than GNSS with fix (potentially `±5-20 ms` without PPS, `±1-2 s` with current second-truncated parsing). The desired hierarchy is `GNSS > SNTP > NITZ > unsynced`, with fallback `GNSS loss → NITZ` and SNTP only when Wi-Fi has internet.
- `gps_client.cpp:176` discards the `.s` fractional part of `+CGPSINFO hhmmss.s`, so GNSS sync is second-resolution even though the modem reports sub-second. `gps_service` also `settimeofday(tv_usec=0)` unconditionally — no `adjtime` slew, no forward-only guarantee.
- SNTP servers are hard-coded (`wifi_manager.cpp:14 pool.ntp.org/time.nist.gov`), not configurable, and the lwIP `sntp` daemon runs forever once started — it will overwrite a precise GNSS time.
- No defence against a lying source: a bad NITZ (misconfigured carrier) or GNSS spoofing/jamming (false `+CGPSINFO` fix with hours of offset) can drag the system clock and any NTP clients. A per-source `timeSyncEnabled` flag and quorum-based quarantine are needed: if SNTP and NITZ agree within ~10 s but GNSS differs by hours, GNSS must be temporarily ignored.

Options considered:

- **Keep current `|diff|>2s` GNSS + always-on SNTP + ignore NITZ.** Minimal change, but keeps second-resolution GNSS, never uses NITZ for NTP-less time, and cannot detect spoofed time.
- **Naive priority switch: when modem appears stop SNTP and sync from NITZ on every poll; when GNSS fix appears stop both and sync from GNSS to ms, else fall back to NITZ.** Matches the initial proposal, but stopping SNTP whenever the modem is present degrades accuracy when internet is available (SNTP beats NITZ). Also ignores the need for SNTP configurability and outlier detection.
- **Full NTP discipline (chrony/ntpd) on ESP32.** Accurate but far too heavy for ESP32-S3, needs PPS GPIO (SIM7670G Classic has no exposed 1PPS), and duplicates lwIP SNTP.
- **Unified arbiter with configurable SNTP, per-source enables, forward-only slew/step, and quorum quarantine (chosen).** Smallest change that satisfies stratum 1, preserves YAGNI, and reuses existing poll loops.

## Decision

Introduce a unified time-sync layer (`system/time_sync.*`) that owns the system clock and NTP stratum:

- **Sources and priority (highest first):** `kGnss` (stratum 1, dispersion 100-200 ms without PPS), `kSntp` (stratum 2-3), `kNitz` (stratum 3-4), `kUnsynced`. Priority is evaluated on *freshness* (`lastSyncAge < 2×gpsPollInterval+10 s` for GNSS — default `130 s` for 60 s poll; `<2 h` for SNTP; `<5 min` for NITZ) not mere interface presence. Fixed `30 s` would be stale on every default GNSS cycle, so the window is tied to the configured poll interval.
- **Lifecycle control:** Wi-Fi `onStationConnected` no longer calls `configTime()` directly; `TimeSync` starts/stops the lwIP SNTP daemon (`sntp_init/stop` via `configTime`) when the selected source changes. GNSS and modem polls feed `TimeSample{source, epochMs, receivedMs}` into `TimeSync`; only `TimeSync` calls `settimeofday`/`adjtime`.
- **Clock discipline:** forward-only. `|diff| < small` → `adjtime` slew forward; `diff > forwardThreshold` → `settimeofday` step forward; negative `diff` beyond deadband is ignored (or logged) unless quorum proves the current clock is wrong. No backward steps during normal operation.
- **GNSS precision:** preserve `+CGPSINFO` fractional seconds (stop stripping `'.s'`), capture `ms`, and compensate for `AT` latency when computing `epochMs`. Report honest `root dispersion` (UART jitter) to NTP clients.
- **Configurability:**
  - `RuntimeConfig` extended with `ntpServer1/2` (0-64 printable, empty = disabled) and `ntpEnabled`; `ConfigRecord` version bump with migration.
  - `RuntimeGpsConfig` gains `timeSyncEnabled bool` (separate from `enabled`).
  - `RuntimeModemSourceConfig` gains `nitzTimeSyncEnabled bool`.
  - Both new flags version-bump their respective records.
  - All three flags default `true` and are exposed in the protected web UI (Digest) alongside existing poll intervals.
- **Spoof / bad-source defence (quorum):** when at least two sources are fresh and agree within `kQuorumAgreeSec` (~10 s), their mean is the quorum. A third source differing by `> kQuarantineSec` (~300 s) is quarantined for `kQuarantineDuration` (15 min, exponential backoff on repeat) with `event=time_quarantine source=gnss reason=quorum_mismatch`. Quarantined samples are not used for discipline; UI/API shows `quarantinedUntil`.
- **API/observability:** `GET /api/time` and `WebStatus` expose `timeSource, lastSyncMs, stratum, dispersionMs, quarantined`. Serial events `event=time_sync source=...`, `event=time_quarantine`, `event=gps_time_sync` (now with `ms`), `event=sntp_begin/stop`.
- **NTP server:** enable lwIP `NTP server` (or minimal UDP 123 responder) that serves the disciplined system clock with `stratum`/`dispersion` from `TimeSync`. When `kUnsynced`, serve stratum 0 / no service or stratum 16 per RFC.

USB recovery contract (`appcfg` erase `0x610000 0x6000`) and `partitions.csv` unchanged.

## Alternatives Considered

### Keep `|diff|>2s` and always-on SNTP, ignore NITZ

No migration, but GNSS stays second-resolution, NITZ never helps offline, and a spoofed GNSS can permanently poison the clock and NTP clients. Rejected: does not meet stratum 1 or reliability requirements.

### Stop SNTP whenever modem is present; GNSS overrides all, fallback to NITZ

Simple, matches the first draft of the proposal. Rejected because SNTP is strictly more accurate than NITZ when internet is available; stopping it degrades time for all clients. Also lacks configurability and outlier detection.

### Full NTP implementation (chrony-like) on ESP32

Would give best discipline and PPS support but exceeds flash/RAM budget, requires hardware PPS not available on this board revision, and conflicts with YAGNI. Deferred.

### Per-source hard disable only (no quorum)

Operator can manually disable NITZ/GNSS time sync, but cannot automatically survive a transient spoof without manual intervention. Quorum adds automatic protection with small code cost, so it is included.

## Consequences

- **Positive:** time source is observable (`source/lastSync/stratum`), SNTP servers are operator-configurable, NITZ becomes a useful offline fallback, GNSS provides ms-resolution time, and a single `TimeSync` owns all `settimeofday` calls with forward-only safety. Spoofed or misconfigured sources are automatically quarantined when a quorum exists, while manual per-source toggles cover persistent bad operators.
- **Negative / trade-offs:** three record version bumps and NVS migrations; `sntp` start/stop adds lifecycle complexity; quorum thresholds (`10 s` / `300 s` / `15 min`) are heuristic and need field tuning; without PPS, advertised stratum 1 still carries `100-200 ms` dispersion from `AT` latency — clients must be told honestly.
- **Accepted risks:** `adjtime` slew on ESP32 is coarse; large forward jumps still step; SNTP without NTS/Autokey remains spoofable (mitigated by quorum with NITZ/GNSS); GNSS jamming that yields *no fix* is handled by fallback, but sophisticated spoofing that also spoofs NITZ/SNTP could still fool the quorum — mitigated by `nitzTimeSyncEnabled`/`gnss.timeSyncEnabled` manual overrides; NTP server increases UDP exposure — remains behind the trusted LAN assumption shared with the ZTE modem's plain-HTTP API.
