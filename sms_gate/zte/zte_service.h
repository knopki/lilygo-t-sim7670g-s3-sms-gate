// #region MODULE_CONTRACT
// PURPOSE: Owns the ZTE MF79RU source lifecycle (NVS profile, poll,
// test and send) so the main firmware only calls sync/load/save/status.
// SCOPE:
// - ZteConfigStore load/save, WebZteConfig snapshot, form validation,
//   poll interval, status cache, the zte_poll/test/send FreeRTOS tasks,
//   forwardZteSms via SMTP and the goform dialog.
// - NOT: Wi-Fi lifecycle, SIM7670G modem, HTTP route registration.
// INVARIANTS: Incoming SMS deleted only after SMTP 250 OK; outgoing
// records cleaned only after terminal status; credentials never logged;
// at most one poll/test/send owns the modem at a time.
// DEPENDENCIES: Uses ZteClient, ZteTransport, SmtpClient, SmtpTransport,
// WifiManager for ehlo/mDNS, ConfigStore and WebApi.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_SERVICE_H
#define ZTE_ZTE_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store.h"
#include "system/task_control.h"
#include "system/web_api.h"
#include "zte/zte_client.h"

class SmtpService;
class WifiManager;

// #region CLASS_ZteService
// PURPOSE: Encapsulates the ZTE source state and its async tasks.
class ZteService {
 public:
  ZteService();
  bool load();
  bool save(const RuntimeZteConfig& candidate);
  bool isLoaded() const { return loaded_; }
  const RuntimeZteConfig& config() const { return stored_; }
  WebZteConfig webConfig() const;
  bool readForm(WebServer& server, RuntimeZteConfig& out, String& error);
  bool readSendForm(WebServer& server, String& to, String& text, String& error);
  unsigned long pollIntervalMs() const;
  String lastStatus() const;
  void publishStatus(const char* status);

  bool isPollCycleActive() const { return pollCycleActive_; }
  bool isTestRunning() const { return testRunning_; }
  bool isSendRunning() const { return sendRunning_; }
  WebAsyncOp testStatus() const;
  WebAsyncOp sendStatus() const;
  bool startTest(const RuntimeZteConfig& candidate, String& error);
  bool startSend(const String& to, const String& text, String& error);
  void syncPollTask(bool shouldRun);
  void setSmtpService(SmtpService* smtp) { smtp_ = smtp; }
  void setWifiManager(WifiManager* wifi) { wifi_ = wifi; }
  bool shouldRunPoll(bool smtpReady) const;

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
