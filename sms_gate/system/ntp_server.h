// #region MODULE_CONTRACT
// PURPOSE: Minimal RFC 5905 NTP server serving TimeSync stratum/dispersion
// (ADR-0005) on UDP 123 while STA is connected.
// SCOPE:
// - WiFiUDP socket on 123, 48-byte NTP request parsing, TX timestamp from
//   gettimeofday, stratum/dispersion/rootDelay from TimeSync, no service
//   when stratum 0 (unsynced).
// - NOT: SNTP client, clock discipline, HTTP, persistence.
// INVARIANTS: Only serves when WiFi connected and stratum>0; never steps
// clock; stratum/dispersion from TimeSync::state().
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
};

#endif  // SYSTEM_NTP_SERVER_H
