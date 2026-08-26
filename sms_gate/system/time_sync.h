// #region MODULE_CONTRACT
// PURPOSE: Owns wall-clock arbitration (GNSS > SNTP > NITZ), SNTP lifecycle,
// forward-only discipline and NTP stratum so the rest of the firmware only
// feeds samples (ADR-0005).
// SCOPE:
// - TimeSource/TimeState, sample feeding (GNSS/NITZ/SNTP), freshness
//   check (GNSS 2×poll+10s, SNTP <2h, NITZ <5min), quorum quarantine
//   (|agree|<10s, outlier >300s → 15min quarantine), adjtime/settimeofday
//   forward-only, SNTP start/stop and NTP server stratum/dispersion.
// - NOT: Wi-Fi STA/AP state machine, modem AT, GNSS AT, HTTP routes, NVS
//   persistence (config flags live in ConfigStore/GpsStore/ModemStore).
// INVARIANTS: Only TimeSync calls settimeofday/adjtime; clock never steps
// backward in normal operation; quarantined sources are ignored for
// discipline; NTP stratum 0/16 when unsynced.
// DEPENDENCIES: Uses <time.h>/<sys/time.h>, esp_sntp, Wi-Fi poll intervals
// for freshness windows; no credential handling.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_TIME_SYNC_H
#define SYSTEM_TIME_SYNC_H

#include <Arduino.h>
#include <stdint.h>
#include <sys/time.h>

enum class TimeSource : uint8_t {
  kUnsynced = 0,
  kSntp = 1,
  kNitz = 2,
  kGnss = 3,
};

struct TimeSample {
  TimeSource source = TimeSource::kUnsynced;
  int64_t epochMs = 0;      // UTC ms since epoch
  uint32_t receivedMs = 0;  // millis() when sample was captured
  uint32_t accuracyMs = 0;  // estimated 1σ (GNSS ~100, SNTP ~50, NITZ ~1500)
  bool valid = false;
};

struct TimeState {
  TimeSource source = TimeSource::kUnsynced;
  int64_t epochMs = 0;
  uint32_t lastSyncMs = 0;    // millis() of last accepted sync
  uint8_t stratum = 0;        // 0 unsynced, 1 GNSS, 2-3 SNTP, 3-4 NITZ
  uint32_t dispersionMs = 0;  // root dispersion for NTP
  bool quarantined = false;
  uint32_t quarantinedUntilMs = 0;
};

// #region CLASS_TimeSync
// PURPOSE: Single owner of the system clock and NTP stratum (ADR-0005).
class TimeSync {
 public:
  void begin();

  // Feeders — called from GNSS/modem/Wi-Fi pollers (portMUX protected inside).
  void feedGnssSample(int64_t epochMs, uint32_t accuracyMs);
  void feedNitzSample(int64_t epochMs, uint32_t accuracyMs);
  void feedSntpSync(int64_t epochMs);
  // Test hooks with explicit nowMs (host-testable).
  void feedGnssSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs);
  void feedNitzSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs);
  void feedSntpSyncAt(int64_t epochMs, uint32_t nowMs);

  // Periodic arbitration + discipline (called from loop or dedicated task).
  void loop();
  void loopAt(uint32_t nowMs, int64_t wallMs);

  // NTP server helpers.
  const TimeState& state() const { return published_; }
  uint8_t stratum() const;
  const char* sourceName() const;
  const char* sourceName(TimeSource s) const;

  // SNTP lifecycle (WifiManager delegates here per ADR-0005).
  void startSntp(const char* server1, const char* server2);
  void stopSntp();

  // Poll interval for GNSS freshness (default 60s).
  void setGpsPollMs(uint32_t ms) { gpsPollMs_ = ms; }
  void setModemPollMs(uint32_t ms) { modemPollMs_ = ms; }

  // Freshness windows (exposed for tests).
  static bool isGnssFresh(uint32_t ageMs, uint32_t gpsPollMs);
  static bool isSntpFresh(uint32_t ageMs);
  static bool isNitzFresh(uint32_t ageMs);

  // For host tests: inspect quarantine.
  bool isQuarantined(TimeSource s, uint32_t nowMs) const;

 private:
  TimeSource arbitrateAt(uint32_t nowMs) const;
  // cppcheck-suppress unusedPrivateFunction
  TimeSource arbitrate();
  // Returns wall diff ms (chosen - wall) and performs forward-only discipline.
  int64_t disciplineAt(const TimeSample& chosen, int64_t wallMs);
  // cppcheck-suppress unusedPrivateFunction
  void discipline(const TimeSample& chosen);
  bool shouldQuarantineAt(const TimeSample& sample, uint32_t nowMs);
  // cppcheck-suppress unusedPrivateFunction
  bool shouldQuarantine(const TimeSample& sample);

  TimeSample samples_[4] = {};
  TimeState published_ = {};
  uint32_t quarantineUntilMs_[4] = {};

  uint32_t gpsPollMs_ = 60UL * 1000UL;
  uint32_t modemPollMs_ = 15UL * 1000UL;
  bool sntpRunning_ = false;
};
// #endregion CLASS_TimeSync

#endif  // SYSTEM_TIME_SYNC_H
