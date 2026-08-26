// #region MODULE_CONTRACT
// PURPOSE: Owns the onboard SIM7670G source lifecycle (NVS profile,
// status poll, SMS poll/forward and AT send) so the main firmware only
// drives sync and HTTP delegation.
// SCOPE:
// - ModemSourceStore load/save, WebModemSourceConfig snapshot, form
//   validation, poll interval, status cache, the modem_poll/send tasks,
//   forwardModemSms via SMTP and AT dialog.
// - NOT: Wi-Fi lifecycle, ZTE goform, HTTP route registration.
// INVARIANTS: SMS deleted only after SMTP 250 OK; Serial1 owned only by
// modem tasks; credentials never logged; at most one poll owns Serial1.
// DEPENDENCIES: Uses ModemClient, ModemTransport, SmtpClient, SmtpTransport,
// WifiManager, ConfigStore and WebApi.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_SERVICE_H
#define MODEM_MODEM_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store.h"
#include "modem/modem_client.h"
#include "system/task_control.h"
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
  WebAsyncOp sendStatus() const;
  bool startSend(const String& to, const String& text, String& error);
  void syncTask();
  bool stopTask();

  void setSmtpService(class SmtpService* smtp) { smtp_ = smtp; }
  void setWifiManager(class WifiManager* wifi) { wifi_ = wifi; }
  void setZteService(class ZteService* zte) { zte_ = zte; }

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

  class SmtpService* smtp_ = nullptr;
  class WifiManager* wifi_ = nullptr;
  class ZteService* zte_ = nullptr;

  static void pollTask(void* param);
  static void sendTask(void* param);
  void runPollTask();
  void runSend();
  bool shouldRunSms(const ModemStatus& snapshot) const;
  bool forwardSms(const ModemSms& sms);
  void runPollCycle(ModemClient& client);
};
// #endregion CLASS_ModemService
#endif  // MODEM_MODEM_SERVICE_H
