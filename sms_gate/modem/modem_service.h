// #region MODULE_CONTRACT
// PURPOSE: Keeps SIM7670G polling and forwarding outside the firmware shell.
// SCOPE:
// - Profile persistence, status projection, polling, sending, and concat cleanup.
// - NOT: Wi-Fi, ZTE, HTTP routes, or persistent multipart state.
// INVARIANTS: Serial1 has one owner; SMTP acceptance precedes deletion; secrets never enter logs.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_SERVICE_H
#define MODEM_MODEM_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store_modem.h"
#include "modem/concat_cache.h"
#include "modem/modem_client.h"
#include "system/task_control.h"
#include "system/time_sync.h"
#include "system/web_api.h"

// #region CLASS_ModemService
// PURPOSE: Keeps SIM7670G lifecycle decisions out of the firmware shell.
class ModemService {
 public:
  ModemService();
  // #region METHOD_ModemService_load
  // PURPOSE: Makes saved modem policy available before task control.
  bool load();
  // #endregion METHOD_ModemService_load

  // #region METHOD_ModemService_save
  // PURPOSE: Keeps validated modem policy across reboot.
  bool save(const RuntimeModemSourceConfig& candidate);
  // #endregion METHOD_ModemService_save
  bool isLoaded() const { return loaded_; }
  const RuntimeModemSourceConfig& config() const { return stored_; }
  // #region METHOD_ModemService_webSourceConfig
  // PURPOSE: Lets the UI inspect modem policy without store access.
  WebModemSourceConfig webSourceConfig() const;
  // #endregion METHOD_ModemService_webSourceConfig

  // #region METHOD_ModemService_webStatus
  // PURPOSE: Lets the UI inspect modem state without Serial1 access.
  WebModemStatus webStatus() const;
  // #endregion METHOD_ModemService_webStatus

  // #region METHOD_ModemService_readSourceForm
  // PURPOSE: Prevents invalid modem policy from reaching persistence.
  bool readSourceForm(WebServer& server, RuntimeModemSourceConfig& out, String& error);
  // #endregion METHOD_ModemService_readSourceForm
  // #region METHOD_ModemService_pollIntervalMs
  // PURPOSE: Converts the profile interval into a safe scheduler delay.
  unsigned long pollIntervalMs() const;
  // #endregion METHOD_ModemService_pollIntervalMs

  // #region METHOD_ModemService_publishStatus
  // PURPOSE: Publishes a modem snapshot without exposing Serial1.
  void publishStatus(const ModemStatus& status);
  // #endregion METHOD_ModemService_publishStatus

  // #region METHOD_ModemService_readStatus
  // PURPOSE: Gives consumers one consistent modem snapshot.
  ModemStatus readStatus() const;
  // #endregion METHOD_ModemService_readStatus

  bool isPollCycleActive() const { return pollCycleActive_; }
  bool isSendRunning() const { return sendRunning_; }

  // #region METHOD_ModemService_shouldRunTask
  // PURPOSE: Gates task creation on a loaded, enabled modem profile.
  bool shouldRunTask() const;
  // #endregion METHOD_ModemService_shouldRunTask

  // #region METHOD_ModemService_shouldPoll
  // PURPOSE: Gates polling on the profile's module and poll switches.
  bool shouldPoll() const;
  // #endregion METHOD_ModemService_shouldPoll

  // #region METHOD_ModemService_shouldTimeSync
  // PURPOSE: Gates NITZ feeding on an active modem poll profile.
  bool shouldTimeSync() const;
  // #endregion METHOD_ModemService_shouldTimeSync

  // #region METHOD_ModemService_shouldRunSms
  // PURPOSE: Gates SMS forwarding on the profile's SMS switch.
  bool shouldRunSms() const;
  // #endregion METHOD_ModemService_shouldRunSms
  // #region METHOD_ModemService_sendStatus
  // PURPOSE: Lets the UI follow sends without touching Serial1.
  WebAsyncOp sendStatus() const;
  // #endregion METHOD_ModemService_sendStatus

  // #region METHOD_ModemService_startSend
  // PURPOSE: Starts a send without colliding with modem polling.
  bool startSend(const String& to, const String& text, String& error);
  // #endregion METHOD_ModemService_startSend

  // #region METHOD_ModemService_syncTask
  // PURPOSE: Applies modem profile changes without rebooting while safe mode blocks polling.
  void syncTask();
  // #endregion METHOD_ModemService_syncTask

  // #region METHOD_ModemService_stopTask
  // PURPOSE: Releases Serial1 before another operation needs it.
  bool stopTask();
  // #endregion METHOD_ModemService_stopTask

  void setSmtpService(class SmtpService* smtp) { smtp_ = smtp; }
  void setWifiManager(class WifiManager* wifi) { wifi_ = wifi; }
  void setZteService(class ZteService* zte) { zte_ = zte; }
  void setTimeSync(TimeSync* timeSync) { timeSync_ = timeSync; }

 private:
  ModemSourceStore store_;
  RuntimeModemSourceConfig stored_;
  bool loaded_ = false;

  task_control::StatusCache<ModemStatus> statusCache_;

  TaskHandle_t taskHandle_ = nullptr;
  volatile bool taskStopRequested_ = false;
  volatile bool pollCycleActive_ = false;

  String sendTo_;
  String sendText_;
  volatile bool sendRunning_ = false;
  volatile bool sendDone_ = false;
  bool sendSuccess_ = false;
  String sendMessage_;
  ModemConcatCache concatCache_;

  class SmtpService* smtp_ = nullptr;
  class WifiManager* wifi_ = nullptr;
  class ZteService* zte_ = nullptr;
  TimeSync* timeSync_ = nullptr;

  static void pollTask(void* param);
  static void sendTask(void* param);
  void runPollTask();
  void runSend();
  bool shouldRunSms(const ModemStatus& snapshot) const;
  bool forwardSms(const ModemSms& sms);
  bool forwardCompleteConcat(ModemClient& client, size_t setIndex);
  bool forwardExpiredConcat(ModemClient& client, size_t setIndex);
  // Retries only source records already accepted by SMTP and not yet deleted.
  bool deleteConcatSet(ModemClient& client, size_t setIndex);
  void runPollCycle(ModemClient& client);
};
// #endregion CLASS_ModemService
#endif  // MODEM_MODEM_SERVICE_H
