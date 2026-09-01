// #region MODULE_CONTRACT
// PURPOSE: Makes trusted synchronized time available to connected LAN clients.
// SCOPE:
// - Serves bounded NTP client requests with TimeSync quality and rate
// limiting.
// - NOT: SNTP, clock discipline, HTTP, persistence, or hardware time.
// INVARIANTS:
// - Serves only while connected and synchronized;
// - never steps the clock;
// - drains every datagram;
// - unsupported/rate-limited requests get KoD.
// DEPENDENCIES: WiFiUDP, TimeSync, gettimeofday.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_NTP_SERVER_H
#define SYSTEM_NTP_SERVER_H

#include <Arduino.h>

#include "system/time_sync.h"

// #region CLASS_NtpServer
// PURPOSE: Publishes synchronized local time to LAN clients without sharing
// socket or packet logic with the control loop.
class NtpServer {
 public:
  explicit NtpServer(TimeSync& timeSync) : timeSync_(timeSync) {}

  // #region METHOD_NtpServer_begin
  // PURPOSE: Prevents unsynchronized or offline state from exposing an NTP service.
  void begin();
  // #endregion METHOD_NtpServer_begin

  // #region METHOD_NtpServer_loop
  // PURPOSE: Keeps NTP service responsive without monopolizing the firmware loop.
  void loop();
  // #endregion METHOD_NtpServer_loop

 private:
  TimeSync& timeSync_;
  bool started_ = false;
  uint32_t rateWindow_ = 0;  // millis()/1000 of the current rate window
  uint32_t rateCount_ = 0;   // requests seen in the current window
  uint32_t lastLogMs_ = 0;   // last throttled per-request log
};
// #endregion CLASS_NtpServer

#endif  // SYSTEM_NTP_SERVER_H
