// #region MODULE_CONTRACT
// PURPOSE: Owns the SMTP delivery profile and its asynchronous test
// lifecycle so the firmware can validate form input, persist the record
// and run STARTTLS/implicit-TLS dialogs without blocking loop().
// SCOPE:
// - SmtpConfigStore load/save, WebSmtpConfig snapshot, form validation
//   (host/port/user/password/addresses), security/mode mapping, result
//   names, stage logging and the smtp_test FreeRTOS task.
// - NOT: Wi-Fi lifecycle, ZTE/modem dialogs, HTTP route registration
//   and email rendering.
// INVARIANTS: The password is never serialized or logged; every dialog
// failure is traceable to one stage plus reply code; at most one test
// task runs at a time.
// DEPENDENCIES: Uses Arduino-ESP32 WebServer, SmtpClient,
// SecureSmtpChannel, ConfigStore and WebApi helpers.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SMTP_SMTP_SERVICE_H
#define SMTP_SMTP_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store.h"
#include "smtp/smtp_client.h"
#include "system/web_api.h"

void logSmtpStage(const char* stage, int code);

// #region CLASS_SmtpService
// PURPOSE: Encapsulates the stored SMTP profile and the one-at-a-time
// test delivery so callers only interact with form/data snapshots.
class SmtpService {
 public:
  SmtpService() = default;
  bool load();
  bool save(const RuntimeSmtpConfig& candidate);
  bool isLoaded() const { return loaded_; }
  const RuntimeSmtpConfig& config() const { return stored_; }
  WebSmtpConfig webConfig() const;
  bool parseSmtpPort(const String& raw, SmtpSecurityMode mode, uint16_t& port) const;
  bool readSmtpForm(WebServer& server, RuntimeSmtpConfig& candidate, String& error);
  bool startTest(const RuntimeSmtpConfig& candidate, const String& ehloName, String& error);
  WebAsyncOp testStatus() const;

  bool isTestRunning() const { return testRunning_; }
  bool isTestDone() const { return testDone_; }
  void setTestDone(bool done) { testDone_ = done; }

 private:
  SmtpConfigStore store_;
  RuntimeSmtpConfig stored_;
  bool loaded_ = false;

  RuntimeSmtpConfig testCandidate_;
  String testEhloName_;
  volatile bool testRunning_ = false;
  volatile bool testDone_ = false;
  SmtpSendResult testResult_ = SmtpSendResult::kConnectFailed;
  String testMessage_;
  String testFailedStage_;
  int testReplyCode_ = 0;

  static void testTask(void* param);
  void runTest();
};
// #endregion CLASS_SmtpService
#endif  // SMTP_SMTP_SERVICE_H
