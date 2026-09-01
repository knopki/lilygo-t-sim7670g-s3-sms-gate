// #region MODULE_CONTRACT
// PURPOSE: Keeps ZTE polling, forwarding, and tests outside the firmware shell.
// SCOPE:
// - Profile persistence, status projection, polling, sending, and cleanup.
// - NOT: Wi-Fi, SIM7670G, HTTP routes, or SMTP implementation.
// INVARIANTS:
// - SMTP acceptance precedes deletion; one operation owns the modem;
// - secrets never enter logs.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_SERVICE_H
#define ZTE_ZTE_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store_zte.h"
#include "system/task_control.h"
#include "system/web_api.h"
#include "zte/zte_client.h"

class SmtpService;
class WifiManager;

// #region CLASS_ZteService
// PURPOSE: Keeps ZTE lifecycle decisions out of the firmware shell.
class ZteService {
 public:
  ZteService();
  // #region METHOD_ZteService_load
  // PURPOSE: Makes saved ZTE policy available before task control.
  bool load();
  // #endregion METHOD_ZteService_load

  // #region METHOD_ZteService_save
  // PURPOSE: Keeps validated ZTE policy across reboot.
  bool save(const RuntimeZteConfig& candidate);
  // #endregion METHOD_ZteService_save
  bool isLoaded() const { return loaded_; }
  const RuntimeZteConfig& config() const { return stored_; }
  // #region METHOD_ZteService_webConfig
  // PURPOSE: Lets the UI inspect ZTE policy without store access.
  WebZteConfig webConfig() const;
  // #endregion METHOD_ZteService_webConfig

  // #region METHOD_ZteService_readForm
  // PURPOSE: Prevents invalid ZTE policy from reaching persistence.
  bool readForm(WebServer& server, RuntimeZteConfig& out, String& error);
  // #endregion METHOD_ZteService_readForm

  // #region METHOD_ZteService_readSendForm
  // PURPOSE: Prevents malformed outbound SMS from reaching the modem.
  bool readSendForm(WebServer& server, String& to, String& text, String& error);
  // #endregion METHOD_ZteService_readSendForm
  // #region METHOD_ZteService_pollIntervalMs
  // PURPOSE: Converts the profile interval into a safe scheduler delay.
  unsigned long pollIntervalMs() const;
  // #endregion METHOD_ZteService_pollIntervalMs

  // #region METHOD_ZteService_lastStatus
  // PURPOSE: Gives the UI the latest secret-free source outcome.
  String lastStatus() const;
  // #endregion METHOD_ZteService_lastStatus

  // #region METHOD_ZteService_publishStatus
  // PURPOSE: Publishes a ZTE outcome without exposing task internals.
  void publishStatus(const char* status);
  // #endregion METHOD_ZteService_publishStatus

  bool isPollCycleActive() const { return pollCycleActive_; }
  bool isTestRunning() const { return testRunning_; }
  bool isSendRunning() const { return sendRunning_; }
  // #region METHOD_ZteService_testStatus
  // PURPOSE: Lets the UI follow connection tests without blocking routes.
  WebAsyncOp testStatus() const;
  // #endregion METHOD_ZteService_testStatus

  // #region METHOD_ZteService_sendStatus
  // PURPOSE: Lets the UI follow sends without blocking routes.
  WebAsyncOp sendStatus() const;
  // #endregion METHOD_ZteService_sendStatus

  // #region METHOD_ZteService_startTest
  // PURPOSE: Tests ZTE connectivity without colliding with other modem work.
  bool startTest(const RuntimeZteConfig& candidate, String& error);
  // #endregion METHOD_ZteService_startTest

  // #region METHOD_ZteService_startSend
  // PURPOSE: Starts a send without colliding with ZTE polling.
  bool startSend(const String& to, const String& text, String& error);
  // #endregion METHOD_ZteService_startSend

  // #region METHOD_ZteService_syncPollTask
  // PURPOSE: Applies ZTE profile changes without rebooting the gateway.
  void syncPollTask(bool shouldRun);
  // #endregion METHOD_ZteService_syncPollTask
  void setSmtpService(SmtpService* smtp) { smtp_ = smtp; }
  void setWifiManager(WifiManager* wifi) { wifi_ = wifi; }

  // #region METHOD_ZteService_shouldRunModule
  // PURPOSE: Gates all ZTE work on a loaded, enabled profile.
  bool shouldRunModule() const;
  // #endregion METHOD_ZteService_shouldRunModule

  // #region METHOD_ZteService_shouldRunPoll
  // PURPOSE: Gates polling on source readiness and forwarding dependencies.
  bool shouldRunPoll(bool smtpReady) const;
  // #endregion METHOD_ZteService_shouldRunPoll

 private:
  ZteConfigStore store_;
  RuntimeZteConfig stored_;
  bool loaded_ = false;

  // Status cache for the web UI (160 bytes, portMUX).
  static constexpr size_t kStatusLength = 160;
  task_control::StringStatusCache<kStatusLength> statusCache_;

  // Poll lifecycle
  TaskHandle_t pollHandle_ = nullptr;
  volatile bool pollStopRequested_ = false;
  volatile bool pollCycleActive_ = false;

  // Test lifecycle
  RuntimeZteConfig testCandidate_;
  volatile bool testRunning_ = false;
  volatile bool testDone_ = false;
  bool testSuccess_ = false;
  String testMessage_;

  // Send lifecycle
  String sendTo_;
  String sendText_;
  volatile bool sendRunning_ = false;
  volatile bool sendDone_ = false;
  bool sendSuccess_ = false;
  String sendMessage_;
  SmtpService* smtp_ = nullptr;
  WifiManager* wifi_ = nullptr;

  static void pollTask(void* param);
  static void testTask(void* param);
  static void sendTask(void* param);
  void runPollTask();
  void runTest();
  void runSend();
  bool forwardSms(const ZteSms& sms);
  void runPollCycle(ZteModem& modem);
  String replySnippet(const char* body) const;
};
// #endregion CLASS_ZteService
#endif  // ZTE_ZTE_SERVICE_H
