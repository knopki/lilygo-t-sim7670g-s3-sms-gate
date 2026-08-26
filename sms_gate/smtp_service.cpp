// #region MODULE_CONTRACT
// PURPOSE: Implements the SMTP service owned by SmtpService so the
// monolithic sms_gate.ino only calls load/save/webConfig/readForm/startTest.
// #endregion MODULE_CONTRACT

#include "smtp_service.h"

#include <Arduino.h>

#include "config_store_common.h"
#include "smtp_client.h"
#include "smtp_transport.h"
#include "task_control.h"

namespace {

using task_control::kPollSliceMs;
using task_control::kServiceTaskStack;

// #region FUNC_smtpSecurityName
// PURPOSE: Maps the security mode onto the stable JSON/HTML token.
const char* smtpSecurityName(SmtpSecurityMode mode) {
  return mode == SmtpSecurityMode::kImplicitTls ? "implicit" : "starttls";
}
// #endregion FUNC_smtpSecurityName

// #region FUNC_smtpResultName
// PURPOSE: Backward-compatible alias onto the shared smtp_client helper.
inline const char* smtpResultName(SmtpSendResult result) { return smtpSendResultName(result); }
// #endregion FUNC_smtpResultName

// #region FUNC_smtpResultMessage
// PURPOSE: Translates one outcome into the operator-facing explanation shown
// next to the test button.
String smtpResultMessage(SmtpSendResult result) {
  switch (result) {
    case SmtpSendResult::kSuccess:
      return F("The test message was delivered to the SMTP server.");
    case SmtpSendResult::kConnectFailed:
      return F("Could not reach the SMTP server. Check the host, port, and network.");
    case SmtpSendResult::kTlsUnavailable:
      return F("The server does not offer the required STARTTLS upgrade.");
    case SmtpSendResult::kTlsFailed:
      return F("The TLS handshake failed. The server certificate could not be verified.");
    case SmtpSendResult::kAuthRejected:
      return F("The server rejected the username or password.");
    case SmtpSendResult::kMessageRejected:
      return F("The server rejected the sender or recipient address.");
    default:
      return F("The SMTP dialog ended unexpectedly.");
  }
}
// #endregion FUNC_smtpResultMessage

}  // namespace

// #region FUNC_logSmtpStage
// PURPOSE: Shared stage tracer — defined once for smtp/zte/modem paths.
void logSmtpStage(const char* stage, int code) {
  Serial.printf("event=smtp_stage name=%s code=%d heap=%u max_alloc=%u\n", stage, code,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
}
// #endregion FUNC_logSmtpStage

// #region METHOD_SmtpService_load
// PURPOSE: Loads the stored profile from NVS.
bool SmtpService::load() {
  loaded_ = store_.load(stored_);
  return loaded_;
}
// #endregion METHOD_SmtpService_load

// #region METHOD_SmtpService_save
// PURPOSE: Persists a validated candidate and updates the in-memory copy.
bool SmtpService::save(const RuntimeSmtpConfig& candidate) {
  if (!store_.save(candidate)) {
    return false;
  }
  stored_ = candidate;
  loaded_ = true;
  testDone_ = false;
  testMessage_ = "";
  return true;
}
// #endregion METHOD_SmtpService_save

// #region METHOD_SmtpService_webConfig
// PURPOSE: Snapshots the stored profile for the JSON API without the password.
WebSmtpConfig SmtpService::webConfig() const {
  WebSmtpConfig web;
  web.present = loaded_ && stored_.host.length() > 0;
  web.host = web.present ? stored_.host : String();
  web.port = web.present ? stored_.port
                         : (stored_.securityMode == SmtpSecurityMode::kImplicitTls ? 465 : 587);
  web.security =
      web.present ? String(smtpSecurityName(stored_.securityMode)) : String(F("starttls"));
  web.username = web.present ? stored_.username : String();
  web.passwordSet = web.present && stored_.password.length() > 0;
  web.fromAddress = web.present ? stored_.fromAddress : String();
  web.recipientAddress = web.present ? stored_.recipientAddress : String();
  return web;
}
// #endregion METHOD_SmtpService_webConfig

// #region METHOD_SmtpService_parseSmtpPort
// PURPOSE: Parses a port field or falls back to the mode's default.
bool SmtpService::parseSmtpPort(const String& raw, SmtpSecurityMode mode, uint16_t& port) const {
  String value = raw;
  value.trim();
  if (value.length() == 0) {
    port = mode == SmtpSecurityMode::kImplicitTls ? 465 : 587;
    return true;
  }
  if (value.length() > 5) {
    return false;
  }
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || parsed < 1 || parsed > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(parsed);
  return true;
}
// #endregion METHOD_SmtpService_parseSmtpPort

// #region METHOD_SmtpService_readSmtpForm
// PURPOSE: Validates the SMTP form into a runtime profile.
bool SmtpService::readSmtpForm(WebServer& server, RuntimeSmtpConfig& candidate, String& error) {
  const String security = server.arg("security");
  if (security == F("implicit")) {
    candidate.securityMode = SmtpSecurityMode::kImplicitTls;
  } else if (security == F("starttls")) {
    candidate.securityMode = SmtpSecurityMode::kStartTls;
  } else {
    error = F("Select STARTTLS (587) or implicit TLS (465).");
    return false;
  }

  candidate.host = server.arg("host");
  candidate.host.trim();
  if (candidate.host.length() == 0 || candidate.host.length() > kMaxSmtpHostLength ||
      !isPrintableAscii(candidate.host)) {
    error = F("Server host must contain 1–127 printable ASCII characters.");
    return false;
  }
  if (!parseSmtpPort(server.arg("port"), candidate.securityMode, candidate.port)) {
    error = F("Port must be a number between 1 and 65535.");
    return false;
  }

  candidate.username = server.arg("username");
  candidate.username.trim();
  if (candidate.username.length() == 0 || candidate.username.length() > kMaxSmtpUserLength ||
      !isPrintableAscii(candidate.username)) {
    error = F("Username must contain 1–127 printable ASCII characters.");
    return false;
  }

  candidate.password = server.arg("password");
  if (candidate.password.length() == 0) {
    if (!loaded_ || stored_.password.length() == 0) {
      error = F("Enter the SMTP password.");
      return false;
    }
    candidate.password = stored_.password;
  } else if (candidate.password.length() > kMaxSmtpPasswordLength ||
             !isPrintableAscii(candidate.password)) {
    error = F("SMTP password must contain 1–95 printable ASCII characters.");
    return false;
  }

  candidate.fromAddress = server.arg("from");
  candidate.fromAddress.trim();
  if (candidate.fromAddress.length() == 0 ||
      candidate.fromAddress.length() > kMaxSmtpAddressLength ||
      !isPrintableAscii(candidate.fromAddress) || candidate.fromAddress.indexOf('@') < 0) {
    error = F("From address must be an email address of up to 127 ASCII characters.");
    return false;
  }
  candidate.recipientAddress = server.arg("recipient");
  candidate.recipientAddress.trim();
  if (candidate.recipientAddress.length() == 0 ||
      candidate.recipientAddress.length() > kMaxSmtpAddressLength ||
      !isPrintableAscii(candidate.recipientAddress) ||
      candidate.recipientAddress.indexOf('@') < 0) {
    error = F("Recipient address must be an email address of up to 127 ASCII characters.");
    return false;
  }
  return true;
}
// #endregion METHOD_SmtpService_readSmtpForm

// #region METHOD_SmtpService_startTest
// PURPOSE: Starts one test delivery on its own task.
bool SmtpService::startTest(const RuntimeSmtpConfig& candidate, const String& ehloName,
                            String& error) {
  if (testRunning_) {
    error = F("A test delivery is already in progress.");
    return false;
  }
  testCandidate_ = candidate;
  testEhloName_ = ehloName;
  testDone_ = false;
  testMessage_ = "";
  testRunning_ = true;
  if (xTaskCreatePinnedToCore(testTask, "smtp_test", kServiceTaskStack, this, 1, nullptr, 0) !=
      pdPASS) {
    testRunning_ = false;
    Serial.println("event=smtp_test_failed reason=task_create");
    error = F("The test could not be started. Try again.");
    return false;
  }
  return true;
}
// #endregion METHOD_SmtpService_startTest

// #region METHOD_SmtpService_testStatus
// PURPOSE: Snapshots the async test progress.
WebAsyncOp SmtpService::testStatus() const {
  WebAsyncOp op;
  op.running = testRunning_;
  op.done = testDone_;
  if (testDone_) {
    op.result = smtpResultName(testResult_);
    op.message = testMessage_;
  }
  return op;
}
// #endregion METHOD_SmtpService_testStatus

// #region METHOD_SmtpService_testTask
// PURPOSE: Runs one test delivery on its own task.
void SmtpService::testTask(void* param) {
  auto* self = static_cast<SmtpService*>(param);
  if (self != nullptr) {
    self->runTest();
  }
  vTaskDelete(nullptr);
}
// #endregion METHOD_SmtpService_testTask

// #region METHOD_SmtpService_runTest
// PURPOSE: Executes the blocking SMTP dialog and publishes the outcome last.
void SmtpService::runTest() {
  const RuntimeSmtpConfig candidate = testCandidate_;
  const String ehloName = testEhloName_;
  const unsigned long startedAt = millis();
  Serial.printf("event=smtp_test_begin host=%s port=%u mode=%s heap=%u\n", candidate.host.c_str(),
                candidate.port, smtpSecurityName(candidate.securityMode),
                static_cast<unsigned>(ESP.getFreeHeap()));

  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  const SmtpConfigRecord record = buildSmtpConfigRecord(candidate);
  testResult_ = client.sendMail(record, ehloName.c_str(), "SMS Gate test message",
                                "This is a test message from the SMS Gate device. "
                                "If you can read it, SMTP delivery is working.");
  String message = smtpResultMessage(testResult_);
  if (testResult_ != SmtpSendResult::kSuccess) {
    message += F(" [stage=");
    message += client.failedStage();
    if (client.lastReplyCode() != 0) {
      message += F(" code=");
      message += String(client.lastReplyCode());
    }
    message += ']';
  }
  testMessage_ = message;
  testFailedStage_ = client.failedStage();
  testReplyCode_ = client.lastReplyCode();
  Serial.printf(
      "event=smtp_test_complete result=%s stage=%s code=%d detail=%c errno=%d "
      "elapsed_ms=%lu heap=%u\n",
      smtpResultName(testResult_), client.failedStage(), client.lastReplyCode(),
      channel.readDetail(), channel.lastErrno(), millis() - startedAt,
      static_cast<unsigned>(ESP.getFreeHeap()));
  testRunning_ = false;
  testDone_ = true;
}
// #endregion METHOD_SmtpService_runTest
