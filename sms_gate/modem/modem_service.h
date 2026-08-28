// #region MODULE_CONTRACT
// PURPOSE: Owns the onboard SIM7670G source lifecycle (NVS profile,
// status poll, SMS poll/forward and AT send) so the main firmware only
// drives sync and HTTP delegation.
// SCOPE:
// - ModemSourceStore load/save, WebModemSourceConfig snapshot, form
//   validation, poll interval, status cache, the modem_poll/send tasks,
//   forwardModemSms via SMTP, AT dialog, and the bounded volatile concat
//   cache (two sets, five parts each, 20 poll-cycle incomplete fallback).
// - NOT: Wi-Fi lifecycle, ZTE goform, HTTP route registration or persistent
//   multipart state.
// INVARIANTS: SMS deleted only after SMTP 250 OK; volatile concat state
// records SMTP acceptance before each CMGD and retries only unfinished
// cleanup during this boot; oversized/cache-full parts use the incomplete
// single-message path; Serial1 is owned by modem tasks; credentials never
// logged; at most one poll owns Serial1.
// DEPENDENCIES: Uses ModemClient, ModemTransport, SmtpClient, SmtpTransport,
// WifiManager, ConfigStore and WebApi.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_SERVICE_H
#define MODEM_MODEM_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store.h"
#include "modem/concat_cache.h"
#include "modem/modem_client.h"
#include "system/task_control.h"
#include "system/time_sync.h"
#include "system/web_api.h"

// #region CLASS_ModemService
// PURPOSE: Encapsulates the SIM7670G source state and its tasks.
class ModemService {
 public:
  ModemService();
  bool load();
  bool save(const RuntimeModemSourceConfig& candidate);
  bool isLoaded() const { return loaded_; }
  const RuntimeModemSourceConfig& config() const { return stored_; }
  WebModemSourceConfig webSourceConfig() const;
  WebModemStatus webStatus() const;
  bool readSourceForm(WebServer& server, RuntimeModemSourceConfig& out, String& error);
  unsigned long pollIntervalMs() const;
  void publishStatus(const ModemStatus& status);
  ModemStatus readStatus() const;
  bool isPollCycleActive() const { return pollCycleActive_; }
  bool isSendRunning() const { return sendRunning_; }
  bool shouldRunTask() const;
  bool shouldPoll() const;
  bool shouldTimeSync() const;
  bool shouldRunSms() const;
  WebAsyncOp sendStatus() const;
  bool startSend(const String& to, const String& text, String& error);
  void syncTask();
  bool stopTask();

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
