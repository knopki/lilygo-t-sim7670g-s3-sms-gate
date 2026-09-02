// #region MODULE_CONTRACT
// PURPOSE: Keeps GNSS lifecycle and polling outside the firmware shell.
// SCOPE:
// - Profile persistence, status projection, form validation, and polling.
// - NOT: Wi-Fi, other modem sources, HTTP routes, or clock arbitration.
// INVARIANTS: Serial1 has one owner; status snapshots are consistent; secrets never enter logs.
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
// PURPOSE: Keeps GNSS lifecycle decisions out of the firmware shell.
class GpsService {
 public:
  GpsService();
  // #region METHOD_GpsService_load
  // PURPOSE: Makes the saved GNSS policy available before task control.
  bool load();
  // #endregion METHOD_GpsService_load

  // #region METHOD_GpsService_save
  // PURPOSE: Keeps validated GNSS policy across reboot.
  bool save(const RuntimeGpsConfig& candidate);
  // #endregion METHOD_GpsService_save
  bool isLoaded() const { return loaded_; }
  const RuntimeGpsConfig& config() const { return stored_; }
  // #region METHOD_GpsService_webConfig
  // PURPOSE: Lets the UI inspect GNSS policy without store access.
  WebGpsConfig webConfig() const;
  // #endregion METHOD_GpsService_webConfig

  // #region METHOD_GpsService_webStatus
  // PURPOSE: Lets the UI inspect GNSS state without modem access.
  WebGpsStatus webStatus() const;
  // #endregion METHOD_GpsService_webStatus

  // #region METHOD_GpsService_readForm
  // PURPOSE: Prevents invalid GNSS policy from reaching persistence.
  bool readForm(WebServer& server, RuntimeGpsConfig& out, String& error);
  // #endregion METHOD_GpsService_readForm
  // #region METHOD_GpsService_pollIntervalMs
  // PURPOSE: Converts the profile interval into a safe scheduler delay.
  unsigned long pollIntervalMs() const;
  // #endregion METHOD_GpsService_pollIntervalMs

  // #region METHOD_GpsService_publishStatus
  // PURPOSE: Publishes a GNSS snapshot without exposing the modem channel.
  void publishStatus(const GpsStatus& status);
  // #endregion METHOD_GpsService_publishStatus

  // #region METHOD_GpsService_readStatus
  // PURPOSE: Gives consumers one consistent GNSS snapshot.
  GpsStatus readStatus() const;
  // #endregion METHOD_GpsService_readStatus

  bool isPollActive() const { return pollActive_; }

  // #region METHOD_GpsService_shouldRunTask
  // PURPOSE: Gates task creation on a loaded, enabled GNSS profile.
  bool shouldRunTask() const;
  // #endregion METHOD_GpsService_shouldRunTask

  // #region METHOD_GpsService_shouldPoll
  // PURPOSE: Gates polling on the profile's module and poll switches.
  bool shouldPoll() const;
  // #endregion METHOD_GpsService_shouldPoll

  // #region METHOD_GpsService_shouldTimeSync
  // PURPOSE: Gates GNSS clock feeding on an active poll profile.
  bool shouldTimeSync() const;
  // #endregion METHOD_GpsService_shouldTimeSync

  void setTimeSync(TimeSync* timeSync) { timeSync_ = timeSync; }
  // #region METHOD_GpsService_syncTask
  // PURPOSE: Applies profile changes without rebooting while safe mode blocks polling.
  void syncTask();
  // #endregion METHOD_GpsService_syncTask

  // #region METHOD_GpsService_stopTask
  // PURPOSE: Releases Serial1 before another service needs it.
  bool stopTask();
  // #endregion METHOD_GpsService_stopTask

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
