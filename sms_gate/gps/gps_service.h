// #region MODULE_CONTRACT
// PURPOSE: Owns the GNSS polling lifecycle (NVS profile, status poll and time
// sync) so the main firmware only drives sync and HTTP delegation.
// SCOPE:
// - GpsConfigStore load/save, WebGpsConfig snapshot, form validation,
//   poll interval, status cache, the gps_poll task, and CGNSS dialog via
//   GpsClient (antenna bias + CGNSSPWR + CGNSSMODE + CGPSINFO/CGNSSINFO).
// - NOT: Wi-Fi lifecycle, ZTE goform, SIM SMS, HTTP route registration.
// INVARIANTS: Serial1 accessed only while holding modem_lock mutex;
// status cache is portMUX-protected and never exposes lock across String;
// credentials never logged; at most one GNSS poll owns Serial1.
// DEPENDENCIES: Uses GpsClient, ModemTransport, GpsConfigStore, ConfigStore,
// WebApi and task_control.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef GPS_GPS_SERVICE_H
#define GPS_GPS_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "gps/gps_client.h"
#include "persistence/config_store_gps.h"
#include "system/task_control.h"
#include "system/time_sync.h"
#include "system/web_api.h"

// #region CLASS_GpsService
// PURPOSE: Encapsulates the GNSS source state and its poll task.
class GpsService {
 public:
  GpsService();
  bool load();
  bool save(const RuntimeGpsConfig& candidate);
  bool isLoaded() const { return loaded_; }
  const RuntimeGpsConfig& config() const { return stored_; }
  WebGpsConfig webConfig() const;
  WebGpsStatus webStatus() const;
  bool readForm(WebServer& server, RuntimeGpsConfig& out, String& error);
  unsigned long pollIntervalMs() const;
  void publishStatus(const GpsStatus& status);
  GpsStatus readStatus() const;
  bool isPollActive() const { return pollActive_; }
  void setTimeSync(TimeSync* timeSync) { timeSync_ = timeSync; }
  void syncTask();
  bool stopTask();

 private:
  GpsConfigStore store_;
  RuntimeGpsConfig stored_;
  bool loaded_ = false;

  task_control::StatusCache<GpsStatus> statusCache_;

  TimeSync* timeSync_ = nullptr;
  TaskHandle_t taskHandle_ = nullptr;
  volatile bool taskStopRequested_ = false;
  volatile bool pollActive_ = false;

  static void pollTask(void* param);
  void runPollTask();
};
// #endregion CLASS_GpsService

#endif  // GPS_GPS_SERVICE_H
