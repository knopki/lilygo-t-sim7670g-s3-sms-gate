#include <assert.h>
#include <cstdint>
#include <cstdio>
#include "../sms_gate/system/time_sync.h"
#include "../sms_gate/modem/modem_client.h"
#include "../sms_gate/gps/gps_client.h"

// Host test for TimeSync arbitration, freshness and quorum quarantine (ADR-0005).

static int64_t makeEpochMs(int y, int mo, int d, int h, int mi, int s, int ms = 0) {
  auto daysFromCivil = [](int yy, int mm, int dd) -> int64_t {
    yy -= mm <= 2;
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const int yoe = yy - era * 400;
    const int doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + doe - 719468LL;
  };
  int64_t days = daysFromCivil(y, mo, d);
  int64_t sec = days * 86400LL + h * 3600LL + mi * 60LL + s;
  return sec * 1000LL + ms;
}

void testFreshness() {
  assert(TimeSync::isGnssFresh(0, 60000));
  assert(TimeSync::isGnssFresh(129000, 60000));   // 2*60+10 =130s window, 129 <130
  assert(!TimeSync::isGnssFresh(130000, 60000));
  assert(!TimeSync::isGnssFresh(130001, 60000));
  // Custom poll 5s -> window 20s
  assert(TimeSync::isGnssFresh(19999, 5000));
  assert(!TimeSync::isGnssFresh(20000, 5000));
  assert(TimeSync::isSntpFresh(0));
  assert(TimeSync::isSntpFresh(2UL * 60 * 60 * 1000 - 1));
  assert(!TimeSync::isSntpFresh(2UL * 60 * 60 * 1000));
  assert(TimeSync::isNitzFresh(0));
  assert(TimeSync::isNitzFresh(5UL * 60 * 1000 - 1));
  assert(!TimeSync::isNitzFresh(5UL * 60 * 1000));
  puts("testFreshness ok");
}

void testArbitrationPriority() {
  TimeSync ts;
  ts.begin();
  ts.setGpsPollMs(60000);
  int64_t e = makeEpochMs(2025, 8, 26, 12, 0, 0);
  // Only NITZ -> NITZ
  ts.feedNitzSampleAt(e, 1500, 1000);
  ts.loopAt(2000, e);
  assert(ts.state().source == TimeSource::kNitz);
  // Add SNTP -> SNTP wins over NITZ
  ts.feedSntpSyncAt(e, 3000);
  ts.loopAt(4000, e);
  assert(ts.state().source == TimeSource::kSntp);
  // Add GNSS -> GNSS wins
  ts.feedGnssSampleAt(e, 100, 5000);
  ts.loopAt(6000, e);
  assert(ts.state().source == TimeSource::kGnss);
  puts("testArbitrationPriority ok");
}

void testGnssFreshnessWindow() {
  TimeSync ts;
  ts.begin();
  ts.setGpsPollMs(60000);
  int64_t e = makeEpochMs(2025, 8, 26, 12, 0, 0);
  ts.feedGnssSampleAt(e, 100, 0);
  // Fresh at 129s
  ts.loopAt(129000, e);
  assert(ts.state().source == TimeSource::kGnss);
  // Stale at 130s -> unsynced if no other source
  ts.loopAt(130000, e);
  assert(ts.state().source == TimeSource::kUnsynced);
  puts("testGnssFreshnessWindow ok");
}

void testQuarantineGnssOutlier() {
  TimeSync ts;
  ts.begin();
  ts.setGpsPollMs(60000);
  int64_t base = makeEpochMs(2025, 8, 26, 12, 0, 0);
  int64_t outlier = base + 3600LL * 1000LL;  // +1h
  // SNTP and NITZ agree at base, fresh
  ts.feedSntpSyncAt(base, 1000);
  ts.feedNitzSampleAt(base + 5000, 1500, 2000);  // +5s within 10s agree
  ts.loopAt(3000, base);
  assert(ts.state().source == TimeSource::kSntp);
  // GNSS outlier +1h should be quarantined (diff 1h >300s, peers agree <10s)
  ts.feedGnssSampleAt(outlier, 100, 4000);
  // GNSS quarantined, SNTP still selected
  ts.loopAt(5000, base);
  assert(ts.state().source == TimeSource::kSntp);
  assert(ts.isQuarantined(TimeSource::kGnss, 5000));
  // After 15min quarantine expires, GNSS outlier would be considered again; test expiry
  ts.loopAt(5000 + 15UL * 60 * 1000 + 1, base);
  // Peers are now stale (SNTP age >2h? No, still fresh at 15min, NITZ stale at 5min)
  // Make fresh peers again at new time
  int64_t nowEpoch = base;
  uint32_t nowMs = 5000 + 15UL * 60 * 1000 + 1000;
  ts.feedSntpSyncAt(nowEpoch, nowMs - 1000);
  ts.feedNitzSampleAt(nowEpoch, 1500, nowMs - 500);
  // Quarantine expired, but GNSS outlier still far -> should quarantine again
  ts.feedGnssSampleAt(outlier, 100, nowMs);
  ts.loopAt(nowMs + 10, nowEpoch);
  assert(ts.isQuarantined(TimeSource::kGnss, nowMs + 10));
  puts("testQuarantineGnssOutlier ok");
}

void testQuorumNoQuarantineWhenPeersDisagree() {
  TimeSync ts;
  ts.begin();
  int64_t base = makeEpochMs(2025, 8, 26, 12, 0, 0);
  // SNTP at base, NITZ 1h away -> peers disagree >10s, no quorum, GNSS outlier should NOT be quarantined
  ts.feedSntpSyncAt(base, 1000);
  ts.feedNitzSampleAt(base + 3600LL * 1000, 1500, 2000);
  ts.feedGnssSampleAt(base + 3600LL * 1000 + 10000, 100, 3000);
  ts.loopAt(4000, base);
  // GNSS not quarantined because no quorum
  assert(!ts.isQuarantined(TimeSource::kGnss, 4000));
  puts("testQuorumNoQuarantineWhenPeersDisagree ok");
}

void testCclkToEpoch() {
  int64_t ms = 0;
  // 25/08/25,12:34:56+12 -> +12 quarters = +3h => UTC 09:34:56
  assert(cclkToEpochMs("25/08/25,12:34:56+12", ms));
  int64_t exp = makeEpochMs(2025, 8, 25, 9, 34, 56);
  assert(ms == exp);
  assert(cclkToEpochMs("25/08/25,12:34:56+00", ms));
  assert(ms == makeEpochMs(2025, 8, 25, 12, 34, 56));
  assert(cclkToEpochMs("25/08/25,00:00:00-04", ms));  // -1h
  assert(ms == makeEpochMs(2025, 8, 25, 1, 0, 0));
  assert(!cclkToEpochMs("bad", ms));
  puts("testCclkToEpoch ok");
}

void testGpsFixMs() {
  GpsFixFields f{};
  assert(parseCgpsInfoLine("+CGPSINFO: 5544.1234,N,03736.5678,E,250826,123456.789,100.0,0.0,0.0", f));
  assert(f.hasFix);
  assert(f.timeMs == 789);
  assert(strcmp(f.utcTime, "123456") == 0);
  int64_t epoch = 0;
  // 250826 = 25-08-2026 + 12:34:56.789
  assert(gpsFixToEpochMs(f, epoch));
  assert(epoch == makeEpochMs(2026, 8, 25, 12, 34, 56, 789));
  // Without fractional part ms=0
  assert(parseCgpsInfoLine("+CGPSINFO: 5544.1234,N,03736.5678,E,250826,123456,100.0,0.0,0.0", f));
  assert(f.timeMs == 0);
  // One digit ".5" -> 500
  assert(parseCgpsInfoLine("+CGPSINFO: 5544.1234,N,03736.5678,E,250826,123456.5,100.0,0.0,0.0", f));
  assert(f.timeMs == 500);
  puts("testGpsFixMs ok");
}

int main() {
  testFreshness();
  testArbitrationPriority();
  testGnssFreshnessWindow();
  testQuarantineGnssOutlier();
  testQuorumNoQuarantineWhenPeersDisagree();
  testCclkToEpoch();
  testGpsFixMs();
  puts("all time_sync tests passed");
  return 0;
}
