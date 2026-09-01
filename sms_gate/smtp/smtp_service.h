// #region MODULE_CONTRACT
// PURPOSE: Keeps SMTP validation and tests from blocking gateway control.
// SCOPE:
// - Profile persistence, form validation, status, stage logging, and
// the guarded SMTP test task.
// - NOT: Wi-Fi, modem dialogs, routes, or rendering.
// INVARIANTS:
// - Passwords stay private;
// - failures expose stage and reply only;
// - at most one test task runs at a time.
// DEPENDENCIES: WebServer, SmtpClient, SecureSmtpChannel, ConfigStore, WebApi.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SMTP_SMTP_SERVICE_H
#define SMTP_SMTP_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store_smtp.h"
#include "smtp/smtp_client.h"
#include "system/web_api.h"

// #region FUNC_logSmtpStage
// PURPOSE: Emits one secret-free SMTP stage event for diagnostics.
void logSmtpStage(const char* stage, int code);
// #endregion FUNC_logSmtpStage

// #region CLASS_SmtpService
// PURPOSE: Encapsulates the stored SMTP profile and the one-at-a-time
// test delivery so callers only interact with form/data snapshots.
class SmtpService {
 public:
  SmtpService() = default;
  // #region METHOD_SmtpService_load
  // PURPOSE: Restores the SMTP profile before tests or forwarding.
  bool load();
  // #endregion METHOD_SmtpService_load

  // #region METHOD_SmtpService_save
  // PURPOSE: Persists a validated SMTP profile for future delivery.
  bool save(const RuntimeSmtpConfig& candidate);
  // #endregion METHOD_SmtpService_save
  bool isLoaded() const { return loaded_; }
  const RuntimeSmtpConfig& config() const { return stored_; }
  // #region METHOD_SmtpService_webConfig
  // PURPOSE: Projects stored SMTP settings without exposing the password.
  WebSmtpConfig webConfig() const;
  // #endregion METHOD_SmtpService_webConfig

  // #region METHOD_SmtpService_parseSmtpPort
  // PURPOSE: Applies the security mode's valid SMTP port rule.
  bool parseSmtpPort(const String& raw, SmtpSecurityMode mode, uint16_t& port) const;
  // #endregion METHOD_SmtpService_parseSmtpPort

  // #region METHOD_SmtpService_readSmtpForm
  // PURPOSE: Validates SMTP form input before persistence or testing.
  bool readSmtpForm(WebServer& server, RuntimeSmtpConfig& candidate, String& error);
  // #endregion METHOD_SmtpService_readSmtpForm
  // #region METHOD_SmtpService_startTest
  // PURPOSE: Keeps SMTP verification isolated from gateway control and route timing.
  bool startTest(const RuntimeSmtpConfig& candidate, const String& ehloName, String& error);
  // #endregion METHOD_SmtpService_startTest

  // #region METHOD_SmtpService_testStatus
  // PURPOSE: Lets the UI follow SMTP verification without joining its worker task.
  WebAsyncOp testStatus() const;
  // #endregion METHOD_SmtpService_testStatus

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
