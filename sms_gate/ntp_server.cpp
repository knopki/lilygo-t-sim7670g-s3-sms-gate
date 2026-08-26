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
WiFiUDP udp;

void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = (v >> 24) & 0xFF;
  p[1] = (v >> 16) & 0xFF;
  p[2] = (v >> 8) & 0xFF;
  p[3] = v & 0xFF;
}

uint32_t readU32BE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void addMsToNtp(uint32_t sec, uint32_t ms, uint32_t& outSec, uint32_t& outFrac) {
  outSec = sec + kNtpEpochOffset;
  // frac = ms * 2^32 / 1000
  outFrac = (uint32_t)((uint64_t)ms * 4294967296ULL / 1000ULL);
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
  if (packetSize < (int)kNtpPacketSize) return;

  uint8_t req[kNtpPacketSize] = {};
  udp.read(req, kNtpPacketSize);
  IPAddress remoteIp = udp.remoteIP();
  uint16_t remotePort = udp.remotePort();

  const TimeState st = timeSync_.state();
  if (st.stratum == 0) {
    // Unsynced: do not serve time per ADR-0005 (stratum 0/16).
    Serial.println("event=ntp_request_ignored reason=unsynced");
    return;
  }

  uint8_t resp[kNtpPacketSize] = {};
  // LI=0, VN=4 (from request low 3 bits or 4), Mode=4 (server)
  uint8_t vn = (req[0] >> 3) & 0x07;
  if (vn == 0) vn = 4;
  resp[0] = (vn << 3) | 4;  // LI=0
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

  struct timeval tv{};
  gettimeofday(&tv, nullptr);
  uint32_t txSec = 0;
  uint32_t txFrac = 0;
  addMsToNtp((uint32_t)tv.tv_sec, tv.tv_usec / 1000, txSec, txFrac);

  // ref timestamp: lastSync or tx if no ref
  uint32_t refSec = txSec;
  uint32_t refFrac = txFrac;
  if (st.epochMs > 0) {
    addMsToNtp((uint32_t)(st.epochMs / 1000), (uint32_t)(st.epochMs % 1000), refSec, refFrac);
  }
  writeU32BE(resp + 16, refSec);
  writeU32BE(resp + 20, refFrac);

  // orig timestamp: copy transmit timestamp from request (bytes 40-47)
  resp[24] = req[40];
  resp[25] = req[41];
  resp[26] = req[42];
  resp[27] = req[43];
  resp[28] = req[44];
  resp[29] = req[45];
  resp[30] = req[46];
  resp[31] = req[47];

  // recv timestamp: same as tx for minimal server
  writeU32BE(resp + 32, txSec);
  writeU32BE(resp + 36, txFrac);
  // tx timestamp
  writeU32BE(resp + 40, txSec);
  writeU32BE(resp + 44, txFrac);

  udp.beginPacket(remoteIp, remotePort);
  udp.write(resp, kNtpPacketSize);
  udp.endPacket();
  (void)readU32BE;  // keep helper referenced for host builds

  Serial.printf("event=ntp_served stratum=%u src=%s to=%s\n", st.stratum,
                timeSync_.sourceName(st.source), remoteIp.toString().c_str());
#else
  (void)timeSync_;
#endif
}
// #endregion METHOD_NtpServer_loop
