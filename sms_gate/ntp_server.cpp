#include "system/ntp_server.h"

#ifdef ARDUINO
#include <WiFi.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>
#else
// Host stub: no UDP on host tests.
#endif

#ifdef ARDUINO
namespace {
constexpr uint16_t kNtpPort = 123;
constexpr size_t kNtpPacketSize = 48;
constexpr uint32_t kNtpEpochOffset = 2208988800UL;  // 1900->1970
constexpr uint8_t kNtpModeClient = 3;
// Global reply budget per second. Above it the server answers with a
// Kiss-o'-Death RATE so compliant clients (chrony/ntpd) back off and the
// shared Arduino loop (modem/SMS/HTTP polling) survives NTP floods.
constexpr uint32_t kNtpRateLimitPerSecond = 20;
constexpr uint32_t kNtpLogThrottleMs = 1000;
WiFiUDP udp;

void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF;
  p[1] = (v >> 16) & 0xFF;
  p[2] = (v >> 8) & 0xFF;
  p[3] = v & 0xFF;
}

void addMsToNtp(uint32_t sec, uint32_t ms, uint32_t& outSec, uint32_t& outFrac) {
  outSec = sec + kNtpEpochOffset;
  // frac = ms * 2^32 / 1000
  outFrac = (uint32_t)((uint64_t)ms * 4294967296ULL / 1000ULL);
}

// t2/t3 use the full microsecond part of gettimeofday: ms quantization
// reads as ~1 ms clock noise to NTP clients while precision says 2^-20.
void stampNow(uint32_t& outSec, uint32_t& outFrac) {
  struct timeval tv{};
  gettimeofday(&tv, nullptr);
  outSec = (uint32_t)tv.tv_sec + kNtpEpochOffset;
  // frac = us * 2^32 / 1e6
  outFrac = (uint32_t)((uint64_t)tv.tv_usec * 4294967296ULL / 1000000ULL);
}
}  // namespace
#endif

// #region METHOD_NtpServer_begin
void NtpServer::begin() {
#ifdef ARDUINO
  if (started_) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!udp.begin(kNtpPort)) {
    Serial.println("event=ntp_server_failed port=123");
    return;
  }
  started_ = true;
  Serial.println("event=ntp_server_started port=123");
#else
  started_ = true;
#endif
}
// #endregion METHOD_NtpServer_begin

// #region METHOD_NtpServer_loop
void NtpServer::loop() {
#ifdef ARDUINO
  if (WiFi.status() != WL_CONNECTED) {
    if (started_) {
      udp.stop();
      started_ = false;
      Serial.println("event=ntp_server_stopped reason=sta_down");
    }
    return;
  }
  if (!started_) {
    begin();
    if (!started_) return;
  }
  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  const uint32_t nowMs = millis();
  uint8_t req[kNtpPacketSize] = {};
  const int got = udp.read(req, kNtpPacketSize);
  // Drain any remainder NOW: NetworkUDP::parsePacket() returns 0 forever
  // while the previous datagram is unread (rx_buffer check), so a stray
  // short (<48 B) or oversized (>48 B) datagram would otherwise deafen the
  // server until reset (observed: HTTP alive, NTP silent).
  udp.flush();
  IPAddress remoteIp = udp.remoteIP();
  uint16_t remotePort = udp.remotePort();
  if (packetSize < (int)kNtpPacketSize || got < (int)kNtpPacketSize) return;

  const TimeState st = timeSync_.state();
  if (st.stratum == 0) {
    // Unsynced: do not serve time per ADR-0005 (stratum 0/16).
    if (nowMs - lastLogMs_ >= kNtpLogThrottleMs) {
      lastLogMs_ = nowMs;
      Serial.println("event=ntp_request_ignored reason=unsynced");
    }
    return;
  }

  // Anti-reflection: answer client mode only. Replying to anything else
  // (e.g. a spoofed mode-4 server response) invites reflection loops
  // between servers.
  if ((req[0] & 0x07) != kNtpModeClient) {
    if (nowMs - lastLogMs_ >= kNtpLogThrottleMs) {
      lastLogMs_ = nowMs;
      Serial.println("event=ntp_request_ignored reason=mode");
    }
    return;
  }

  // Global rate window keyed on millis(): the wall clock may still be
  // unsynced or stale. Over-budget requests get a Kiss-o'-Death RATE.
  if (nowMs / 1000 != rateWindow_) {
    rateWindow_ = nowMs / 1000;
    rateCount_ = 0;
  }
  ++rateCount_;
  const bool overBudget = rateCount_ > kNtpRateLimitPerSecond;

  // Echo the client's version clamped to NTPv3..v4; higher values are bogus.
  uint8_t vn = (req[0] >> 3) & 0x07;
  if (vn < 3) vn = 3;
  if (vn > 4) vn = 4;

  // t2: stamped right after the request was drained.
  uint32_t t2Sec = 0;
  uint32_t t2Frac = 0;
  stampNow(t2Sec, t2Frac);

  uint8_t resp[kNtpPacketSize] = {};
  if (overBudget) {
    // Kiss-o'-Death RATE. chrony/ntpd require LI=3 together with stratum 0
    // before honoring a kiss code, and the origin timestamp must stay the
    // client's or the KoD is discarded as spoofed (which would defeat the
    // rate limiting). Root delay/dispersion are meaningless at stratum 0.
    resp[0] = (3u << 6) | (vn << 3) | 4u;
    resp[1] = 0;  // stratum 0
    resp[2] = 6;
    resp[3] = (int8_t)-20;
    resp[12] = 'R';
    resp[13] = 'A';
    resp[14] = 'T';
    resp[15] = 'E';
  } else {
    resp[0] = (vn << 3) | 4;  // LI=0 (synced: stratum>0 checked above)
    resp[1] = st.stratum;
    resp[2] = 6;            // poll
    resp[3] = (int8_t)-20;  // precision ~1us (2^-20)
    // root delay/dispersion: 32-bit fixed point 16.16; dispersion from TimeSync
    uint32_t disp16 = (uint32_t)st.dispersionMs * 65536 / 1000;
    writeU32BE(resp + 4, 0);  // root delay 0
    writeU32BE(resp + 8, disp16);
    // ref ID: GPS./LOCL/PPS. etc.
    const char* refId = "GPS.";
    if (st.source == TimeSource::kSntp)
      refId = ".GPS.";
    else if (st.source == TimeSource::kNitz)
      refId = ".LOCL";
    resp[12] = refId[0];
    resp[13] = refId[1];
    resp[14] = refId[2];
    resp[15] = refId[3];

    // ref timestamp: lastSync or t2 if no ref
    uint32_t refSec = t2Sec;
    uint32_t refFrac = t2Frac;
    if (st.epochMs > 0) {
      addMsToNtp((uint32_t)(st.epochMs / 1000), (uint32_t)(st.epochMs % 1000), refSec, refFrac);
    }
    writeU32BE(resp + 16, refSec);
    writeU32BE(resp + 20, refFrac);
  }

  // orig timestamp: copy transmit timestamp from request (bytes 40-47)
  resp[24] = req[40];
  resp[25] = req[41];
  resp[26] = req[42];
  resp[27] = req[43];
  resp[28] = req[44];
  resp[29] = req[45];
  resp[30] = req[46];
  resp[31] = req[47];

  if (overBudget) {
    // KoD timestamps: ntpd rejects a KoD unless org == rec == xmt == the
    // request's transmit timestamp ("inconsistent xmt/org/rec" -> ignored
    // as forged); chrony only needs them nonzero. Copying the origin into
    // rec/xmt satisfies both clients.
    for (size_t i = 0; i < 8; ++i) {
      resp[32 + i] = req[40 + i];
      resp[40 + i] = req[40 + i];
    }
  } else {
    // recv timestamp (t2)
    writeU32BE(resp + 32, t2Sec);
    writeU32BE(resp + 36, t2Frac);
    // tx timestamp (t3): stamped as late as possible, right before the send.
    uint32_t t3Sec = 0;
    uint32_t t3Frac = 0;
    stampNow(t3Sec, t3Frac);
    writeU32BE(resp + 40, t3Sec);
    writeU32BE(resp + 44, t3Frac);
  }

  udp.beginPacket(remoteIp, remotePort);
  udp.write(resp, kNtpPacketSize);
  udp.endPacket();

  // Per-request logs are throttled so an NTP flood cannot flood Serial or
  // allocate a String per packet.
  if (nowMs - lastLogMs_ >= kNtpLogThrottleMs) {
    lastLogMs_ = nowMs;
    Serial.printf("event=ntp_served stratum=%u src=%s to=%s\n", resp[1],
                  timeSync_.sourceName(st.source), remoteIp.toString().c_str());
  }
#else
  (void)timeSync_;
#endif
}
// #endregion METHOD_NtpServer_loop
