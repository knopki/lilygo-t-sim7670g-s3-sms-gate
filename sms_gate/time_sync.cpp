#include "system/time_sync.h"

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
namespace {
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

bool TimeSync::isGnssFresh(uint32_t ageMs, uint32_t gpsPollMs) {
  const uint32_t window = 2 * gpsPollMs + 10UL * 1000UL;
  return ageMs < window;
}

bool TimeSync::isSntpFresh(uint32_t ageMs) { return ageMs < kSntpFreshMs; }

bool TimeSync::isNitzFresh(uint32_t ageMs) { return ageMs < kNitzFreshMs; }

void TimeSync::begin() {
  // cppcheck-suppress useStlAlgorithm
  for (auto& s : samples_) s = TimeSample{};
  // cppcheck-suppress useStlAlgorithm
  for (auto& q : quarantineUntilMs_) q = 0;
  for (auto& d : quarantineDurationMs_) d = kQuarantineDurationMs;
  // cppcheck-suppress useStlAlgorithm
  for (auto& l : lastIgnoredLogMs_) l = 0;
  published_ = TimeState{};
  sntpRunning_ = false;
  gpsPollMs_ = 60UL * 1000UL;
  modemPollMs_ = 15UL * 1000UL;
}

bool TimeSync::isQuarantined(TimeSource s, uint32_t nowMs) const {
  uint8_t idx = static_cast<uint8_t>(s);
  return quarantineUntilMs_[idx] != 0 && nowMs < quarantineUntilMs_[idx];
}

// #region METHOD_TimeSync_shouldQuarantineAt
bool TimeSync::shouldQuarantineAt(const TimeSample& sample, uint32_t nowMs) {
  uint8_t idx = static_cast<uint8_t>(sample.source);
  if (quarantineUntilMs_[idx] != 0 && nowMs < quarantineUntilMs_[idx]) return true;

  // Collect fresh, valid, non-quarantined peers (excluding sample itself).
  TimeSample peers[3];
  size_t peerCount = 0;
  for (uint8_t i = 1; i < 4; ++i) {
    if (i == idx) continue;
    const TimeSample& p = samples_[i];
    if (!p.valid) continue;
    if (quarantineUntilMs_[i] != 0 && nowMs < quarantineUntilMs_[i]) continue;
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
    // exponential backoff on repeat (capped at 2h)
    uint32_t next = dur * 2;
    if (next > kQuarantineMaxMs) next = kQuarantineMaxMs;
    quarantineDurationMs_[idx] = next;
#ifdef ARDUINO
    Serial.printf(
        "event=time_quarantine source=%s diff_ms=%lld quorum_ms=%lld reason=quorum_mismatch "
        "quarantined_until_ms=%u\n",
        sourceName(sample.source), (long long)diff, (long long)quorum,
        (unsigned)quarantineUntilMs_[idx]);
#else
    (void)quorum;
#endif
    return true;
  }
  return false;
}

bool TimeSync::shouldQuarantine(const TimeSample& sample) {
  return shouldQuarantineAt(sample, millis());
}
// #endregion METHOD_TimeSync_shouldQuarantineAt

void TimeSync::feedGnssSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs) {
  TimeSample s;
  s.source = TimeSource::kGnss;
  s.epochMs = epochMs;
  s.receivedMs = nowMs;
  s.accuracyMs = accuracyMs;
  s.valid = epochMs > 0;
  if (shouldQuarantineAt(s, nowMs)) return;
  samples_[static_cast<uint8_t>(TimeSource::kGnss)] = s;
}

void TimeSync::feedNitzSampleAt(int64_t epochMs, uint32_t accuracyMs, uint32_t nowMs) {
  TimeSample s;
  s.source = TimeSource::kNitz;
  s.epochMs = epochMs;
  s.receivedMs = nowMs;
  s.accuracyMs = accuracyMs;
  s.valid = epochMs > 0;
  if (shouldQuarantineAt(s, nowMs)) return;
  samples_[static_cast<uint8_t>(TimeSource::kNitz)] = s;
}

void TimeSync::feedSntpSyncAt(int64_t epochMs, uint32_t nowMs) {
  TimeSample s;
  s.source = TimeSource::kSntp;
  s.epochMs = epochMs;
  s.receivedMs = nowMs;
  s.accuracyMs = 50;
  s.valid = epochMs > 0;
  if (shouldQuarantineAt(s, nowMs)) return;
  samples_[static_cast<uint8_t>(TimeSource::kSntp)] = s;
}

void TimeSync::feedGnssSample(int64_t epochMs, uint32_t accuracyMs) {
  feedGnssSampleAt(epochMs, accuracyMs, millis());
}

void TimeSync::feedNitzSample(int64_t epochMs, uint32_t accuracyMs) {
  feedNitzSampleAt(epochMs, accuracyMs, millis());
}

void TimeSync::feedSntpSync(int64_t epochMs) { feedSntpSyncAt(epochMs, millis()); }

// #region METHOD_TimeSync_arbitrateAt
TimeSource TimeSync::arbitrateAt(uint32_t nowMs) const {
  const TimeSource order[] = {TimeSource::kGnss, TimeSource::kSntp, TimeSource::kNitz};
  for (TimeSource src : order) {
    uint8_t idx = static_cast<uint8_t>(src);
    const TimeSample& s = samples_[idx];
    if (!s.valid) continue;
    if (quarantineUntilMs_[idx] != 0 && nowMs < quarantineUntilMs_[idx]) continue;
    uint32_t age = nowMs - s.receivedMs;
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

TimeSource TimeSync::arbitrate() { return arbitrateAt(millis()); }
// #endregion METHOD_TimeSync_arbitrateAt

// #region METHOD_TimeSync_disciplineAt
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
// #endregion METHOD_TimeSync_disciplineAt

void TimeSync::loopAt(uint32_t nowMs, int64_t wallMs) {
  // Clear expired quarantines and reset backoff once quarantine lifts.
  for (uint8_t i = 1; i < 4; ++i) {
    if (quarantineUntilMs_[i] != 0 && nowMs >= quarantineUntilMs_[i]) {
      quarantineUntilMs_[i] = 0;
      quarantineDurationMs_[i] = kQuarantineDurationMs;
    }
  }
  TimeSource best = arbitrateAt(nowMs);
  if (best == TimeSource::kUnsynced) {
    published_.source = TimeSource::kUnsynced;
    published_.stratum = 0;
    published_.dispersionMs = 0;
    published_.quarantined = false;
    published_.quarantinedUntilEpochMs = 0;
    return;
  }
  const TimeSample& s = samples_[static_cast<uint8_t>(best)];
  disciplineAt(s, wallMs, nowMs);
  published_.source = best;
  published_.lastSyncEpochMs = s.epochMs;
  // Live "now": sample epoch extrapolated by its age (same formula as
  // disciplineAt) so consumers see a ticking clock between polls.
  published_.epochMs = s.epochMs + (int64_t)(nowMs - s.receivedMs);
  if (best == TimeSource::kGnss) {
    published_.stratum = 1;
    published_.dispersionMs = 150;
  } else if (best == TimeSource::kSntp) {
    published_.stratum = 2;
    published_.dispersionMs = 50;
  } else {
    published_.stratum = 3;
    published_.dispersionMs = 1500;
  }
  // Publish quarantine state for current best source.
  uint8_t bidx = static_cast<uint8_t>(best);
  if (quarantineUntilMs_[bidx] != 0 && nowMs < quarantineUntilMs_[bidx]) {
    published_.quarantined = true;
    published_.quarantinedUntilEpochMs =
        published_.epochMs + (int64_t)(quarantineUntilMs_[bidx] - nowMs);
  } else {
    // If best is not quarantined but any source is, still report max quarantine for observability.
    uint32_t maxQ = 0;
    bool any = false;
    for (uint8_t i = 1; i < 4; ++i) {
      if (quarantineUntilMs_[i] != 0 && nowMs < quarantineUntilMs_[i]) {
        any = true;
        if (quarantineUntilMs_[i] > maxQ) maxQ = quarantineUntilMs_[i];
      }
    }
    published_.quarantined = any;
    published_.quarantinedUntilEpochMs = any ? published_.epochMs + (int64_t)(maxQ - nowMs) : 0;
  }
}

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

uint8_t TimeSync::stratum() const { return published_.stratum; }

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

const char* TimeSync::sourceName() const { return sourceName(published_.source); }

void TimeSync::startSntp(const char* server1, const char* server2) {
  if (server1 == nullptr || server1[0] == '\0') return;
#ifdef ARDUINO
  configTime(0, 0, server1, server2 && server2[0] ? server2 : nullptr);
  sntpRunning_ = true;
  Serial.printf("event=sntp_begin server=%s\n", server1);
#else
  (void)server2;
  sntpRunning_ = true;
#endif
}

void TimeSync::stopSntp() {
  if (!sntpRunning_) return;
#ifdef ARDUINO
  esp_sntp_stop();
  Serial.println("event=sntp_stop");
#endif
  sntpRunning_ = false;
}
