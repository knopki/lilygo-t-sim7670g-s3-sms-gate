// #region MODULE_CONTRACT
// PURPOSE: Minimal RFC 5905 NTP server serving TimeSync stratum/dispersion
// (ADR-0005) on UDP 123 while STA is connected.
// SCOPE:
// - WiFiUDP socket on 123, 48-byte NTP client-mode (3) requests only,
//   t2/t3 stamped at microsecond resolution from gettimeofday,
//   stratum/dispersion/rootDelay from TimeSync, global per-second reply
//   budget answered with Kiss-o'-Death RATE (LI=3 + stratum 0), no time
//   service when stratum 0 (unsynced).
// - NOT: SNTP client, clock discipline, HTTP, persistence, per-client rate
//   buckets, interleaved mode (RFC 9769), hardware timestamping.
// INVARIANTS: Only serves when WiFi connected and stratum>0; never steps
// clock; stratum/dispersion from TimeSync::state(); answers mode-3 requests
// only (anti-reflection); KoD echoes the client transmit timestamp in
// org/rec/xmt (ntpd requirement) and zeroes root delay/dispersion;
// per-request logs throttled to 1/s.
// DEPENDENCIES: WiFiUDP, TimeSync, gettimeofday.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_NTP_SERVER_H
#define SYSTEM_NTP_SERVER_H

#include <Arduino.h>

#include "system/time_sync.h"

class NtpServer {
 public:
  explicit NtpServer(TimeSync& timeSync) : timeSync_(timeSync) {}
  void begin();
  void loop();

 private:
  TimeSync& timeSync_;
  bool started_ = false;
  uint32_t rateWindow_ = 0;  // millis()/1000 of the current rate window
  uint32_t rateCount_ = 0;   // requests seen in the current window
  uint32_t lastLogMs_ = 0;   // last throttled per-request log
};

#endif  // SYSTEM_NTP_SERVER_H
