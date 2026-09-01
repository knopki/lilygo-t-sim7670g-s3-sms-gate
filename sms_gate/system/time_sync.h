// #region MODULE_CONTRACT
// PURPOSE: Keeps the shared clock monotonic while selecting the best source.
// SCOPE:
// - Source arbitration, freshness/quarantine, forward-only discipline,
// SNTP lifecycle, and NTP quality publication.
// - NOT: Wi-Fi state, modem/GNSS dialogs, HTTP routes, or persistence.
// INVARIANTS:
// - Only TimeSync disciplines time;
// - backward steps are rejected;
// - unsynchronized state reports stratum 0.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_TIME_SYNC_H
#define SYSTEM_TIME_SYNC_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <stdint.h>
#include <sys/time.h>

// #region ENUM_TimeSource
// PURPOSE: Keeps source selection order explicit and reviewable.
enum class TimeSource : uint8_t {
  kUnsynced = 0,
  kSntp = 1,
  kNitz = 2,
  kGnss = 3,
};
// #endregion ENUM_TimeSource

// #region STRUCT_TimeSample
// PURPOSE: Gives arbitration one comparable source observation.
struct TimeSample {
  TimeSource source = TimeSource::kUnsynced;
  int64_t epochMs = 0;      // UTC ms since epoch
  uint32_t receivedMs = 0;  // millis() when sample was captured
  uint32_t accuracyMs = 0;  // estimated 1σ (GNSS ~100, SNTP ~50, NITZ ~1500)
  bool valid = false;
};
// #endregion STRUCT_TimeSample

// #region STRUCT_TimeState
// PURPOSE: Gives consumers time quality without exposing arbitration state.
struct TimeState {
  TimeSource source = TimeSource::kUnsynced;
  int64_t epochMs = 0;          // extrapolated UTC now (sample epoch + age)
  int64_t lastSyncEpochMs = 0;  // UTC epoch of the last accepted sync sample
  uint8_t stratum = 0;          // 0 unsynced, 1 GNSS, 2-3 SNTP, 3-4 NITZ
  uint32_t dispersionMs = 0;    // root dispersion for NTP
  bool quarantined = false;
  int64_t quarantinedUntilEpochMs = 0;  // UTC epoch when quarantine lifts
};
// #endregion STRUCT_TimeState

// #region CLASS_TimeSync
// PURPOSE: Prevents competing services from disagreeing about system time.
class TimeSync {
 public:
  // #region METHOD_TimeSync_begin
  // PURPOSE: Starts synchronization from a known source state.
  void begin();
  // #endregion METHOD_TimeSync_begin

  // #region METHOD_TimeSync_feedGnssSample
  // PURPOSE: Makes a live GNSS observation available to arbitration.
  void feedGnssSample(int64_t epochMs, uint32_t accuracyMs);
  // #endregion METHOD_TimeSync_feedGnssSample

  // #region METHOD_TimeSync_feedNitzSample
  // PURPOSE: Makes a live NITZ observation available to arbitration.
  void feedNitzSample(int64_t epochMs, uint32_t accuracyMs);
  // #endregion METHOD_TimeSync_feedNitzSample

  // #region METHOD_TimeSync_feedSntpSync
  // PURPOSE: Makes a live SNTP observation available to arbitration.
  void feedSntpSync(int64_t epochMs);
  // #endregion METHOD_TimeSync_feedSntpSync

  // #region METHOD_TimeSync_feedGnssSampleAt
  // PURPOSE: Makes GNSS arbitration tests deterministic.
  void feedGnssSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs);
  // #endregion METHOD_TimeSync_feedGnssSampleAt
  // #region METHOD_TimeSync_feedNitzSampleAt
  // PURPOSE: Makes NITZ arbitration tests deterministic.
  void feedNitzSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs);
  // #endregion METHOD_TimeSync_feedNitzSampleAt
  // #region METHOD_TimeSync_feedSntpSyncAt
  // PURPOSE: Makes SNTP arbitration tests deterministic.
  void feedSntpSyncAt(int64_t epochMs, uint32_t nowMs);
  // #endregion METHOD_TimeSync_feedSntpSyncAt

  // #region METHOD_TimeSync_loop
  // PURPOSE: Keeps the wall clock aligned with the best fresh source.
  void loop();
  // #endregion METHOD_TimeSync_loop
  // #region METHOD_TimeSync_loopAt
  // PURPOSE: Makes arbitration and clock discipline reproducible in tests.
  void loopAt(uint32_t nowMs, int64_t wallMs);
  // #endregion METHOD_TimeSync_loopAt

  // NTP server helpers.
  // #region METHOD_TimeSync_state
  // PURPOSE: Gives consumers one coherent time-quality snapshot.
  TimeState state() const;
  // #endregion METHOD_TimeSync_state
  // #region METHOD_TimeSync_stratum
  // PURPOSE: Gives NTP consumers the current synchronization quality.
  uint8_t stratum() const;
  // #endregion METHOD_TimeSync_stratum
  // #region METHOD_TimeSync_sourceName
  // PURPOSE: Gives operators a stable token for the selected source.
  const char* sourceName() const;
  // #endregion METHOD_TimeSync_sourceName
  // #region METHOD_TimeSync_sourceName_TimeSource
  // PURPOSE: Gives logs and APIs stable names for every source.
  const char* sourceName(TimeSource s) const;
  // #endregion METHOD_TimeSync_sourceName_TimeSource

  // #region METHOD_TimeSync_startSntp
  // PURPOSE: Provides a network fallback when stronger time sources are absent.
  void startSntp(const char* server1, const char* server2);
  // #endregion METHOD_TimeSync_startSntp

  // #region METHOD_TimeSync_stopSntp
  // PURPOSE: Prevents inactive SNTP from competing with selected time.
  void stopSntp();
  // #endregion METHOD_TimeSync_stopSntp

  // Poll interval for GNSS freshness (default 60s).
  // #region METHOD_TimeSync_setGpsPollMs
  // PURPOSE: Keeps GNSS freshness decisions aligned with its polling schedule.
  void setGpsPollMs(uint32_t ms);
  // #endregion METHOD_TimeSync_setGpsPollMs
  // #region METHOD_TimeSync_setModemPollMs
  // PURPOSE: Retains the modem schedule for synchronization configuration.
  void setModemPollMs(uint32_t ms);
  // #endregion METHOD_TimeSync_setModemPollMs

  // #region METHOD_TimeSync_isGnssFresh
  // PURPOSE: Keeps stale GNSS observations out of arbitration.
  static bool isGnssFresh(uint32_t ageMs, uint32_t gpsPollMs);
  // #endregion METHOD_TimeSync_isGnssFresh

  // #region METHOD_TimeSync_isSntpFresh
  // PURPOSE: Keeps stale SNTP observations out of arbitration.
  static bool isSntpFresh(uint32_t ageMs);
  // #endregion METHOD_TimeSync_isSntpFresh

  // #region METHOD_TimeSync_isNitzFresh
  // PURPOSE: Keeps stale NITZ observations out of arbitration.
  static bool isNitzFresh(uint32_t ageMs);
  // #endregion METHOD_TimeSync_isNitzFresh

  // #region METHOD_TimeSync_isQuarantined
  // PURPOSE: Lets callers observe quarantine before using a source.
  bool isQuarantined(TimeSource s, uint32_t nowMs) const;
  // #endregion METHOD_TimeSync_isQuarantined

 private:
  void lockSamples() const;
  void unlockSamples() const;
  TimeSource arbitrateAtLocked(uint32_t nowMs) const;
  TimeSource arbitrateAt(uint32_t nowMs) const;
  // cppcheck-suppress unusedPrivateFunction
  TimeSource arbitrate();
  // Returns wall diff ms (expected - wall) and performs forward-only discipline.
  // expected = chosen.epochMs + (nowMs - chosen.receivedMs) to avoid stale-sample drift.
  int64_t disciplineAt(const TimeSample& chosen, int64_t wallMs, uint32_t nowMs);
  // cppcheck-suppress unusedPrivateFunction
  void discipline(const TimeSample& chosen);
  // #region METHOD_TimeSync_feedSampleAt
  // PURPOSE: Atomically applies quorum handling and publishes one source sample.
  void feedSampleAt(TimeSource source, int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs);
  // #endregion METHOD_TimeSync_feedSampleAt
  bool shouldQuarantineAt(const TimeSample& sample, uint32_t nowMs,
                          int64_t* quarantineDiffMs = nullptr, int64_t* quorumMs = nullptr,
                          uint32_t* quarantineUntilMs = nullptr);
  // cppcheck-suppress unusedPrivateFunction
  bool shouldQuarantine(const TimeSample& sample);

  // Serializes producer tasks with loop arbitration; never held during clock I/O or logging.
  mutable portMUX_TYPE samplesMux_ = portMUX_INITIALIZER_UNLOCKED;
  TimeSample samples_[4] = {};
  TimeState published_ = {};
  uint32_t quarantineUntilMs_[4] = {};
  bool quarantineActive_[4] = {};
  uint32_t quarantineDurationMs_[4] = {};
  uint32_t lastIgnoredLogMs_[4] = {};

  uint32_t gpsPollMs_ = 60UL * 1000UL;
  uint32_t modemPollMs_ = 15UL * 1000UL;
  bool sntpRunning_ = false;
};
// #endregion CLASS_TimeSync

#endif  // SYSTEM_TIME_SYNC_H
