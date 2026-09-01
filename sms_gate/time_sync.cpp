// #region MODULE_CONTRACT
// PURPOSE: Keeps the shared clock monotonic while selecting the best source.
// SCOPE:
// - Arbitrates GNSS, SNTP, and NITZ samples and disciplines the shared clock.
// - NOT: Polling GNSS or modem hardware, or serving NTP packets.
// INVARIANTS:
// - Clock updates never move the published time backwards.
// - A disagreeing source is quarantined only after fresh peers form a quorum.
// #endregion MODULE_CONTRACT

#include "system/time_sync.h"

#include "system/millis_deadline.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>
#else
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>
// Host build: Arduino.h comes from tests/host_stub when time_sync is tested.
// Only ESP-specific stubs missing on host.
inline void configTime(long, long, const char*, const char* = nullptr) {}
inline void esp_sntp_stop() {}
#endif

// #region CONST_timeSyncThresholds
// PURPOSE: Keeps source freshness and quarantine decisions consistent.
namespace {
TimeSync* gTimeSyncForSntp = nullptr;

constexpr uint32_t kQuorumAgreeMs = 10UL * 1000UL;
constexpr int64_t kQuarantineDiffMs = 300LL * 1000LL;
constexpr uint32_t kQuarantineDurationMs = 15UL * 60UL * 1000UL;
constexpr uint32_t kQuarantineMaxMs = 2UL * 60UL * 60UL * 1000UL;  // 2h cap for backoff
constexpr uint32_t kSntpFreshMs = 2UL * 60UL * 60UL * 1000UL;      // 2h
constexpr uint32_t kNitzFreshMs = 5UL * 60UL * 1000UL;             // 5 min
constexpr uint32_t kDisciplineIgnoreLogIntervalMs =
    60UL * 1000UL;  // throttle time_discipline_ignored
}  // namespace
// #endregion CONST_timeSyncThresholds

#ifdef ARDUINO
// lwIP calls this weak hook instead of updating the system clock itself.
// TimeSync is the sole owner of forward-only clock discipline.
extern "C" void sntp_sync_time(struct timeval* tv) {
  if (gTimeSyncForSntp == nullptr || tv == nullptr) return;
  const int64_t epochMs = static_cast<int64_t>(tv->tv_sec) * 1000 + tv->tv_usec / 1000;
  gTimeSyncForSntp->feedSntpSync(epochMs);
}
#endif

void TimeSync::lockSamples() const { portENTER_CRITICAL(&samplesMux_); }

void TimeSync::unlockSamples() const { portEXIT_CRITICAL(&samplesMux_); }

// #region METHOD_TimeSync_isGnssFresh
// PURPOSE: Applies the GNSS freshness window used by source arbitration.
bool TimeSync::isGnssFresh(uint32_t ageMs, uint32_t gpsPollMs) {
  const uint32_t window = 2 * gpsPollMs + 10UL * 1000UL;
  return ageMs < window;
}

// #endregion METHOD_TimeSync_isGnssFresh

// #region METHOD_TimeSync_isSntpFresh
// PURPOSE: Applies the SNTP freshness window used by source arbitration.
bool TimeSync::isSntpFresh(uint32_t ageMs) { return ageMs < kSntpFreshMs; }
// #endregion METHOD_TimeSync_isSntpFresh

// #region METHOD_TimeSync_isNitzFresh
// PURPOSE: Applies the NITZ freshness window used by source arbitration.
bool TimeSync::isNitzFresh(uint32_t ageMs) { return ageMs < kNitzFreshMs; }
// #endregion METHOD_TimeSync_isNitzFresh

// #region METHOD_TimeSync_begin
// PURPOSE: Resets samples and quarantine state before synchronization starts.
void TimeSync::begin() {
  lockSamples();
  // cppcheck-suppress useStlAlgorithm
  for (auto& s : samples_) s = TimeSample{};
  // cppcheck-suppress useStlAlgorithm
  for (auto& q : quarantineUntilMs_) q = 0;
  // cppcheck-suppress useStlAlgorithm
  for (auto& active : quarantineActive_) active = false;
  for (auto& d : quarantineDurationMs_) d = kQuarantineDurationMs;
  // cppcheck-suppress useStlAlgorithm
  for (auto& l : lastIgnoredLogMs_) l = 0;
  published_ = TimeState{};
  gpsPollMs_ = 60UL * 1000UL;
  modemPollMs_ = 15UL * 1000UL;
  unlockSamples();
  sntpRunning_ = false;
  if (gTimeSyncForSntp == this) gTimeSyncForSntp = nullptr;
}
// #endregion METHOD_TimeSync_begin

// #region METHOD_TimeSync_isQuarantined
// PURPOSE: Reports whether a source is temporarily excluded from arbitration.
bool TimeSync::isQuarantined(TimeSource s, uint32_t nowMs) const {
  const uint8_t idx = static_cast<uint8_t>(s);
  lockSamples();
  const bool quarantined =
      quarantineActive_[idx] && !millis_deadline::reached(nowMs, quarantineUntilMs_[idx]);
  unlockSamples();
  return quarantined;
}
// #endregion METHOD_TimeSync_isQuarantined

// #region METHOD_TimeSync_shouldQuarantineAt
// PURPOSE: Quarantines a source only when two fresh peers establish a quorum.
bool TimeSync::shouldQuarantineAt(const TimeSample& sample, uint32_t nowMs,
                                  int64_t* quarantineDiffMs, int64_t* quorumMs,
                                  uint32_t* quarantineUntilMs) {
  uint8_t idx = static_cast<uint8_t>(sample.source);
  if (quarantineActive_[idx] && !millis_deadline::reached(nowMs, quarantineUntilMs_[idx]))
    return true;

  // Collect fresh, valid, non-quarantined peers (excluding sample itself).
  TimeSample peers[3];
  size_t peerCount = 0;
  for (uint8_t i = 1; i < 4; ++i) {
    if (i == idx) continue;
    const TimeSample& p = samples_[i];
    if (!p.valid) continue;
    if (quarantineActive_[i] && !millis_deadline::reached(nowMs, quarantineUntilMs_[i])) continue;
    uint32_t age = nowMs - p.receivedMs;
    bool fresh = false;
    if (p.source == TimeSource::kGnss)
      fresh = isGnssFresh(age, gpsPollMs_);
    else if (p.source == TimeSource::kSntp)
      fresh = isSntpFresh(age);
    else if (p.source == TimeSource::kNitz)
      fresh = isNitzFresh(age);
    if (!fresh) continue;
    peers[peerCount++] = p;
  }
  if (peerCount < 2) return false;  // need at least 2 agreeing peers to form quorum (ADR-0005)
  int64_t minMs = peers[0].epochMs;
  // cppcheck-suppress duplicateAssignExpression
  int64_t maxMs = peers[0].epochMs;
  int64_t sum = 0;
  for (size_t i = 0; i < peerCount; ++i) {
    if (peers[i].epochMs < minMs) minMs = peers[i].epochMs;
    if (peers[i].epochMs > maxMs) maxMs = peers[i].epochMs;
    sum += peers[i].epochMs;
  }
  if (maxMs - minMs > (int64_t)kQuorumAgreeMs) return false;  // peers disagree, no quorum
  int64_t quorum = sum / (int64_t)peerCount;
  int64_t diff = sample.epochMs > quorum ? sample.epochMs - quorum : quorum - sample.epochMs;
  if (diff > kQuarantineDiffMs) {
    uint32_t dur = quarantineDurationMs_[idx];
    if (dur == 0) dur = kQuarantineDurationMs;
    quarantineUntilMs_[idx] = nowMs + dur;
    quarantineActive_[idx] = true;
    // exponential backoff on repeat (capped at 2h)
    uint32_t next = dur * 2;
    if (next > kQuarantineMaxMs) next = kQuarantineMaxMs;
    quarantineDurationMs_[idx] = next;
    if (quarantineDiffMs != nullptr) *quarantineDiffMs = diff;
    if (quorumMs != nullptr) *quorumMs = quorum;
    if (quarantineUntilMs != nullptr) *quarantineUntilMs = quarantineUntilMs_[idx];
    return true;
  }
  return false;
}

// #endregion METHOD_TimeSync_shouldQuarantineAt

// #region METHOD_TimeSync_shouldQuarantine
// PURPOSE: Applies quorum quarantine using the device monotonic clock.
bool TimeSync::shouldQuarantine(const TimeSample& sample) {
  lockSamples();
  const bool quarantined = shouldQuarantineAt(sample, millis());
  unlockSamples();
  return quarantined;
}
// #endregion METHOD_TimeSync_shouldQuarantine

// #region METHOD_TimeSync_feedSampleAt
// PURPOSE: Atomically applies quorum handling and publishes one source sample.
void TimeSync::feedSampleAt(TimeSource source, int64_t epochMs, uint32_t accuracyMs,
                            uint32_t nowMs) {
  TimeSample sample;
  sample.source = source;
  sample.epochMs = epochMs;
  sample.receivedMs = nowMs;
  sample.accuracyMs = accuracyMs;
  sample.valid = epochMs > 0;
  int64_t diffMs = 0;
  int64_t quorumMs = 0;
  uint32_t quarantineUntilMs = 0;
  lockSamples();
  const bool quarantined =
      shouldQuarantineAt(sample, nowMs, &diffMs, &quorumMs, &quarantineUntilMs);
  if (!quarantined) samples_[static_cast<uint8_t>(source)] = sample;
  unlockSamples();
#ifdef ARDUINO
  if (quarantineUntilMs != 0) {
    Serial.printf(
        "event=time_quarantine source=%s diff_ms=%lld quorum_ms=%lld reason=quorum_mismatch "
        "quarantined_until_ms=%u\n",
        sourceName(source), (long long)diffMs, (long long)quorumMs, (unsigned)quarantineUntilMs);
  }
#endif
}
// #endregion METHOD_TimeSync_feedSampleAt

// #region METHOD_TimeSync_feedGnssSampleAt
// PURPOSE: Records a GNSS sample after applying quorum quarantine.
void TimeSync::feedGnssSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs) {
  feedSampleAt(TimeSource::kGnss, epochMs, accuracyMs, nowMs);
}

// #endregion METHOD_TimeSync_feedGnssSampleAt

// #region METHOD_TimeSync_feedNitzSampleAt
// PURPOSE: Records a NITZ sample after applying quorum quarantine.
void TimeSync::feedNitzSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs) {
  feedSampleAt(TimeSource::kNitz, epochMs, accuracyMs, nowMs);
}

// #endregion METHOD_TimeSync_feedNitzSampleAt

// #region METHOD_TimeSync_feedSntpSyncAt
// PURPOSE: Records an SNTP sample after applying quorum quarantine.
void TimeSync::feedSntpSyncAt(int64_t epochMs, uint32_t nowMs) {
  feedSampleAt(TimeSource::kSntp, epochMs, 50, nowMs);
}

// #endregion METHOD_TimeSync_feedSntpSyncAt

// #region METHOD_TimeSync_feedGnssSample
// PURPOSE: Records a GNSS sample using the current monotonic time.
void TimeSync::feedGnssSample(int64_t epochMs, uint32_t accuracyMs) {
  feedGnssSampleAt(epochMs, accuracyMs, millis());
}

// #endregion METHOD_TimeSync_feedGnssSample

// #region METHOD_TimeSync_feedNitzSample
// PURPOSE: Records a NITZ sample using the current monotonic time.
void TimeSync::feedNitzSample(int64_t epochMs, uint32_t accuracyMs) {
  feedNitzSampleAt(epochMs, accuracyMs, millis());
}

// #endregion METHOD_TimeSync_feedNitzSample

// #region METHOD_TimeSync_feedSntpSync
// PURPOSE: Records an SNTP sample using the current monotonic time.
void TimeSync::feedSntpSync(int64_t epochMs) { feedSntpSyncAt(epochMs, millis()); }
// #endregion METHOD_TimeSync_feedSntpSync

// #region METHOD_TimeSync_arbitrateAt
// PURPOSE: Selects the highest-priority fresh source that is not quarantined.
TimeSource TimeSync::arbitrateAtLocked(uint32_t nowMs) const {
  const TimeSource order[] = {TimeSource::kGnss, TimeSource::kSntp, TimeSource::kNitz};
  for (TimeSource src : order) {
    const uint8_t idx = static_cast<uint8_t>(src);
    const TimeSample& s = samples_[idx];
    if (!s.valid) continue;
    if (quarantineActive_[idx] && !millis_deadline::reached(nowMs, quarantineUntilMs_[idx]))
      continue;
    const uint32_t age = nowMs - s.receivedMs;
    bool fresh = false;
    if (src == TimeSource::kGnss)
      fresh = isGnssFresh(age, gpsPollMs_);
    else if (src == TimeSource::kSntp)
      fresh = isSntpFresh(age);
    else if (src == TimeSource::kNitz)
      fresh = isNitzFresh(age);
    if (fresh) return src;
  }
  return TimeSource::kUnsynced;
}

TimeSource TimeSync::arbitrateAt(uint32_t nowMs) const {
  lockSamples();
  const TimeSource source = arbitrateAtLocked(nowMs);
  unlockSamples();
  return source;
}
// #endregion METHOD_TimeSync_arbitrateAt

TimeSource TimeSync::arbitrate() { return arbitrateAt(millis()); }

// #region METHOD_TimeSync_disciplineAt
// PURPOSE: Advances or slews the clock without applying backward corrections.
int64_t TimeSync::disciplineAt(const TimeSample& chosen, int64_t wallMs, uint32_t nowMs) {
  uint32_t ageMs = nowMs - chosen.receivedMs;
  int64_t expectedMs = chosen.epochMs + (int64_t)ageMs;
  int64_t diffMs = expectedMs - wallMs;
  if (diffMs < -2000) {
#ifdef ARDUINO
    uint8_t idx = static_cast<uint8_t>(chosen.source);
    if (idx < 4) {
      uint32_t last = lastIgnoredLogMs_[idx];
      if (last == 0 || (uint32_t)(nowMs - last) >= kDisciplineIgnoreLogIntervalMs) {
        lastIgnoredLogMs_[idx] = nowMs;
        Serial.printf("event=time_discipline_ignored diff_ms=%lld source=%s\n", (long long)diffMs,
                      sourceName(chosen.source));
      }
    }
#else
    (void)nowMs;
#endif
    return diffMs;
  }
  if (diffMs > 2000) {
#ifdef ARDUINO
    struct timeval tv{};
    tv.tv_sec = (time_t)(expectedMs / 1000);
    tv.tv_usec = (suseconds_t)((expectedMs % 1000) * 1000);
    settimeofday(&tv, nullptr);
    Serial.printf("event=time_sync source=%s diff_ms=%lld\n", sourceName(chosen.source),
                  (long long)diffMs);
#else
    (void)chosen;
    (void)nowMs;
#endif
    return diffMs;
  }
  if (diffMs > 0) {
#ifdef ARDUINO
    struct timeval delta{};
    delta.tv_sec = (time_t)(diffMs / 1000);
    delta.tv_usec = (suseconds_t)((diffMs % 1000) * 1000);
    struct timeval old{};
    if (adjtime(&delta, &old) == 0) {
      Serial.printf("event=time_slew source=%s diff_ms=%lld\n", sourceName(chosen.source),
                    (long long)diffMs);
    } else {
      struct timeval tv{};
      tv.tv_sec = (time_t)(expectedMs / 1000);
      tv.tv_usec = (suseconds_t)((expectedMs % 1000) * 1000);
      settimeofday(&tv, nullptr);
      Serial.printf("event=time_sync source=%s diff_ms=%lld fallback=adjtime_failed\n",
                    sourceName(chosen.source), (long long)diffMs);
    }
    (void)nowMs;
#else
    (void)chosen;
    (void)nowMs;
#endif
  }
  return diffMs;
}
// #endregion METHOD_TimeSync_disciplineAt

void TimeSync::discipline(const TimeSample& chosen) {
#ifdef ARDUINO
  struct timeval now{};
  gettimeofday(&now, nullptr);
  int64_t wallMs = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
  uint32_t nowMs = millis();
  disciplineAt(chosen, wallMs, nowMs);
#else
  (void)chosen;
#endif
}

// #region METHOD_TimeSync_loopAt
// PURPOSE: Publishes the selected source and disciplines the wall clock.
void TimeSync::loopAt(uint32_t nowMs, int64_t wallMs) {
  TimeSample selected;
  lockSamples();
  // Clear expired quarantines and reset backoff once quarantine lifts.
  for (uint8_t i = 1; i < 4; ++i) {
    if (quarantineActive_[i] && millis_deadline::reached(nowMs, quarantineUntilMs_[i])) {
      quarantineActive_[i] = false;
      quarantineUntilMs_[i] = 0;
      quarantineDurationMs_[i] = kQuarantineDurationMs;
    }
  }
  const TimeSource best = arbitrateAtLocked(nowMs);
  if (best == TimeSource::kUnsynced) {
    published_.source = TimeSource::kUnsynced;
    published_.stratum = 0;
    published_.dispersionMs = 0;
    published_.quarantined = false;
    published_.quarantinedUntilEpochMs = 0;
    unlockSamples();
    return;
  }
  selected = samples_[static_cast<uint8_t>(best)];
  unlockSamples();

  disciplineAt(selected, wallMs, nowMs);

  TimeState next;
  next.source = best;
  next.lastSyncEpochMs = selected.epochMs;
  // Live "now": sample epoch extrapolated by its age (same formula as
  // disciplineAt) so consumers see a ticking clock between polls.
  next.epochMs = selected.epochMs + (int64_t)(nowMs - selected.receivedMs);
  if (best == TimeSource::kGnss) {
    next.stratum = 1;
    next.dispersionMs = 150;
  } else if (best == TimeSource::kSntp) {
    next.stratum = 2;
    next.dispersionMs = 50;
  } else {
    next.stratum = 3;
    next.dispersionMs = 1500;
  }

  lockSamples();
  // Publish quarantine state for current best source.
  const uint8_t bidx = static_cast<uint8_t>(best);
  if (quarantineActive_[bidx] && !millis_deadline::reached(nowMs, quarantineUntilMs_[bidx])) {
    next.quarantined = true;
    next.quarantinedUntilEpochMs = next.epochMs + (int64_t)(quarantineUntilMs_[bidx] - nowMs);
  } else {
    // If best is not quarantined but any source is, still report max quarantine for observability.
    uint32_t maxRemainingMs = 0;
    bool any = false;
    for (uint8_t i = 1; i < 4; ++i) {
      if (quarantineActive_[i] && !millis_deadline::reached(nowMs, quarantineUntilMs_[i])) {
        any = true;
        const uint32_t remainingMs = quarantineUntilMs_[i] - nowMs;
        if (remainingMs > maxRemainingMs) maxRemainingMs = remainingMs;
      }
    }
    next.quarantined = any;
    next.quarantinedUntilEpochMs = any ? next.epochMs + (int64_t)maxRemainingMs : 0;
  }
  published_ = next;
  unlockSamples();
}
// #endregion METHOD_TimeSync_loopAt

// #region METHOD_TimeSync_loop
// PURPOSE: Runs one arbitration tick against the device wall clock.
void TimeSync::loop() {
  uint32_t nowMs = millis();
#ifdef ARDUINO
  struct timeval now{};
  gettimeofday(&now, nullptr);
  int64_t wallMs = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
#else
  int64_t wallMs = 0;
#endif
  loopAt(nowMs, wallMs);
}
// #endregion METHOD_TimeSync_loop

// #region METHOD_TimeSync_state
// PURPOSE: Gives consumers one coherent time-quality snapshot.
TimeState TimeSync::state() const {
  lockSamples();
  const TimeState snapshot = published_;
  unlockSamples();
  return snapshot;
}
// #endregion METHOD_TimeSync_state

// #region METHOD_TimeSync_setGpsPollMs
// PURPOSE: Keeps GNSS freshness decisions aligned with its polling schedule.
void TimeSync::setGpsPollMs(uint32_t ms) {
  lockSamples();
  gpsPollMs_ = ms;
  unlockSamples();
}
// #endregion METHOD_TimeSync_setGpsPollMs

// #region METHOD_TimeSync_setModemPollMs
// PURPOSE: Retains the modem schedule for synchronization configuration.
void TimeSync::setModemPollMs(uint32_t ms) {
  lockSamples();
  modemPollMs_ = ms;
  unlockSamples();
}
// #endregion METHOD_TimeSync_setModemPollMs

// #region METHOD_TimeSync_stratum
// PURPOSE: Gives NTP consumers the current synchronization quality.
uint8_t TimeSync::stratum() const { return state().stratum; }
// #endregion METHOD_TimeSync_stratum

// #region METHOD_TimeSync_sourceName_TimeSource
// PURPOSE: Maps a source enum to its stable operator-facing token.
const char* TimeSync::sourceName(TimeSource s) const {
  switch (s) {
    case TimeSource::kGnss:
      return "gnss";
    case TimeSource::kSntp:
      return "sntp";
    case TimeSource::kNitz:
      return "nitz";
    default:
      return "unsynced";
  }
}
// #endregion METHOD_TimeSync_sourceName_TimeSource

// #region METHOD_TimeSync_sourceName
// PURPOSE: Gives operators a stable token for the selected time source.
const char* TimeSync::sourceName() const { return sourceName(state().source); }
// #endregion METHOD_TimeSync_sourceName

// #region METHOD_TimeSync_startSntp
// PURPOSE: Starts SNTP against the validated operator server list.
void TimeSync::startSntp(const char* server1, const char* server2) {
  if (server1 == nullptr || server1[0] == '\0') return;
#ifdef ARDUINO
  gTimeSyncForSntp = this;
  configTime(0, 0, server1, server2 && server2[0] ? server2 : nullptr);
  sntpRunning_ = true;
  Serial.printf("event=sntp_begin server=%s\n", server1);
#else
  (void)server2;
  sntpRunning_ = true;
#endif
}

// #endregion METHOD_TimeSync_startSntp

// #region METHOD_TimeSync_stopSntp
// PURPOSE: Stops SNTP so it cannot compete with the selected clock source.
void TimeSync::stopSntp() {
  if (!sntpRunning_) return;
#ifdef ARDUINO
  if (gTimeSyncForSntp == this) gTimeSyncForSntp = nullptr;
  esp_sntp_stop();
  Serial.println("event=sntp_stop");
#endif
  sntpRunning_ = false;
}
// #endregion METHOD_TimeSync_stopSntp
