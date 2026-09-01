// #region MODULE_CONTRACT
// PURPOSE: Keeps ZTE polling, forwarding and tests out of the firmware shell.
// SCOPE:
// - Coordinates stored ZTE policy, polling, SMS forwarding, tests, and cached status.
// - NOT: Implementing ZTE wire parsing, HTTP transport, or SMTP transport.
// INVARIANTS:
// - Saved policy becomes the running policy only after persistence succeeds.
// - Web configuration and status expose no ZTE password.
// #endregion MODULE_CONTRACT

#include "zte/zte_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "persistence/config_store_common.h"
#include "system/email_builder.h"
#include "smtp/smtp_client.h"
#include "smtp/smtp_service.h"
#include "smtp/smtp_transport.h"
#include "system/task_control.h"
#include "system/watchdog.h"
#include "system/wifi_manager.h"
#include "zte/zte_transport.h"

namespace {

using task_control::kPollSliceMs;
using task_control::kServiceTaskStack;

constexpr size_t kZteScratchSize = 20UL * 1024UL;
constexpr int kZteSendStatusAttempts = 20;
constexpr unsigned long kZteSendStatusDelayMs = 1000;
constexpr unsigned long kZteOutgoingCleanupRetryDelayMs = 500;
constexpr unsigned int kZteOutgoingCleanupMaxAttempts = kZteMaxPages * kZtePageSize;
constexpr time_t kEpochSynced = 1577836800;

// #region FUNC_zteResultName
// PURPOSE: Keeps protocol result names stable across logs and the operator API.
const char* zteResultName(ZteResult result) {
  switch (result) {
    case ZteResult::kSuccess:
      return "success";
    case ZteResult::kConnectFailed:
      return "connect_failed";
    case ZteResult::kHttpFailed:
      return "http_failed";
    case ZteResult::kLoginRejected:
      return "login_rejected";
    case ZteResult::kStaleSession:
      return "stale_session";
    case ZteResult::kSendRejected:
      return "send_rejected";
    default:
      return "protocol_error";
  }
}
// #endregion FUNC_zteResultName

// SMTP helpers now shared via smtp_client.h:smtpSendResultName and
// smtp_service.h:logSmtpStage — local duplicates removed (P1).

}  // namespace

ZteService::ZteService() = default;

// #region METHOD_ZteService_load
// PURPOSE: Makes stored ZTE policy available before task decisions.
bool ZteService::load() {
  loaded_ = store_.load(stored_);
  return loaded_;
}
// #endregion METHOD_ZteService_load

// #region METHOD_ZteService_save
// PURPOSE: Keeps validated ZTE policy consistent across NVS and the running task.
bool ZteService::save(const RuntimeZteConfig& candidate) {
  if (!store_.save(candidate)) {
    return false;
  }
  stored_ = candidate;
  loaded_ = true;
  testDone_ = false;
  testMessage_ = "";
  return true;
}
// #endregion METHOD_ZteService_save

// #region METHOD_ZteService_webConfig
// PURPOSE: Snapshots stored profile without password.
WebZteConfig ZteService::webConfig() const {
  WebZteConfig web;
  web.present = loaded_ && stored_.host.length() > 0;
  web.moduleEnabled = web.present && stored_.moduleEnabled;
  web.forwardEnabled = web.present && stored_.forwardEnabled;
  web.host = web.present ? stored_.host : String();
  web.passwordSet = web.present && stored_.password.length() > 0;
  web.label = web.present ? stored_.label : String();
  web.pollIntervalSec = web.present ? stored_.pollIntervalSec : kDefaultZtePollSec;
  web.lastStatus = statusCache_.readString();
  return web;
}
// #endregion METHOD_ZteService_webConfig

// #region METHOD_ZteService_readForm
// PURPOSE: Rejects invalid ZTE input before persistence or modem use.
bool ZteService::readForm(WebServer& server, RuntimeZteConfig& out, String& error) {
  out.moduleEnabled = server.arg("module_enabled") == F("1");
  out.forwardEnabled = server.arg("forward_enabled") == F("1");
  if (!server.hasArg("forward_enabled") && server.hasArg("module_enabled")) {
    out.forwardEnabled = out.moduleEnabled;
  }
  out.host = server.arg("host");
  out.host.trim();
  if (out.host.length() == 0 || out.host.length() > kMaxZteHostLength ||
      !isPrintableAscii(out.host)) {
    error = F("Host must contain 1–63 printable ASCII characters.");
    return false;
  }
  out.password = server.arg("password");
  if (out.password.length() == 0) {
    if (!loaded_ || stored_.password.length() == 0) {
      error = F("Enter the modem web password.");
      return false;
    }
    out.password = stored_.password;
  } else if (out.password.length() > kMaxZtePasswordLength || !isPrintableAscii(out.password)) {
    error = F("The modem web password must contain 1–63 printable ASCII characters.");
    return false;
  }
  out.label = server.arg("label");
  out.label.trim();
  if (out.label.length() > kMaxZteLabelLength || !isPrintableAscii(out.label)) {
    error = F("The phone number or alias must contain up to 31 printable ASCII characters.");
    return false;
  }
  if (!parsePollInterval(server.arg("poll_interval"), out.pollIntervalSec, kMinZtePollSec,
                         kMaxZtePollSec, error)) {
    return false;
  }
  return true;
}
// #endregion METHOD_ZteService_readForm

// #region METHOD_ZteService_readSendForm
// PURPOSE: Rejects unsafe SMS input before it reaches the modem dialog.
bool ZteService::readSendForm(WebServer& server, String& to, String& text, String& error) {
  to = server.arg("to");
  to.trim();
  if (!isValidSmsRecipient(to)) {
    error = F("Recipient must be 3\u201320 digits with an optional leading +.");
    return false;
  }
  text = server.arg("text");
  const size_t units = smsUtf16Units(text.c_str());
  if (units == kSmsInvalidUnits) {
    error = F("The message is not valid UTF-8 text.");
    return false;
  }
  if (units == 0) {
    error = F("Enter the message text.");
    return false;
  }
  if (units > kMaxSmsSendUnits) {
    error = F("The message is too long; the modem accepts at most 335 characters.");
    return false;
  }
  return true;
}
// #endregion METHOD_ZteService_readSendForm

// #region METHOD_ZteService_pollIntervalMs
// PURPOSE: Protects the scheduler from corrupt persisted intervals.
unsigned long ZteService::pollIntervalMs() const {
  const uint16_t sec = loaded_ ? stored_.pollIntervalSec : kDefaultZtePollSec;
  if (!isValidZtePollInterval(sec)) return kDefaultZtePollSec * 1000UL;
  return static_cast<unsigned long>(sec) * 1000UL;
}
// #endregion METHOD_ZteService_pollIntervalMs

// #region METHOD_ZteService_lastStatus
// PURPOSE: Gives the UI the latest secret-free source outcome.
String ZteService::lastStatus() const { return statusCache_.readString(); }
// #endregion METHOD_ZteService_lastStatus

// #region METHOD_ZteService_publishStatus
// PURPOSE: Publishes a ZTE outcome without exposing task internals.
void ZteService::publishStatus(const char* status) { statusCache_.publish(status); }
// #endregion METHOD_ZteService_publishStatus

// #region METHOD_ZteService_shouldRunModule
// PURPOSE: Gates all ZTE work on a loaded, enabled profile.
bool ZteService::shouldRunModule() const {
  return loaded_ && stored_.moduleEnabled && stored_.host.length() > 0;
}
// #endregion METHOD_ZteService_shouldRunModule

// #region METHOD_ZteService_shouldRunPoll
// PURPOSE: Gates polling on source readiness and forwarding dependencies.
bool ZteService::shouldRunPoll(bool smtpReady) const {
  return shouldRunModule() && stored_.forwardEnabled && smtpReady;
}
// #endregion METHOD_ZteService_shouldRunPoll

// #region METHOD_ZteService_testStatus
// PURPOSE: Lets the UI poll connection tests without joining their worker task.
WebAsyncOp ZteService::testStatus() const {
  WebAsyncOp op;
  op.running = testRunning_;
  op.done = testDone_;
  if (testDone_) {
    op.result = testSuccess_ ? "success" : "failed";
    op.message = testMessage_;
  }
  return op;
}
// #endregion METHOD_ZteService_testStatus

// #region METHOD_ZteService_sendStatus
// PURPOSE: Lets the UI poll sends without joining their worker task.
WebAsyncOp ZteService::sendStatus() const {
  WebAsyncOp op;
  op.running = sendRunning_;
  op.done = sendDone_;
  if (sendDone_) {
    op.result = sendSuccess_ ? "success" : "failed";
    op.message = sendMessage_;
  }
  return op;
}
// #endregion METHOD_ZteService_sendStatus

// #region METHOD_ZteService_startTest
// PURPOSE: Keeps connection tests non-blocking and single-flight.
bool ZteService::startTest(const RuntimeZteConfig& candidate, String& error) {
  if (testRunning_) {
    error = F("A connection test is already in progress.");
    return false;
  }
  if (sendRunning_) {
    error = F("An SMS send is in progress; try again in a few seconds.");
    return false;
  }
  if (pollCycleActive_) {
    error = F("A poll cycle is in progress; try again in a few seconds.");
    return false;
  }
  testCandidate_ = candidate;
  testDone_ = false;
  testMessage_ = "";
  testRunning_ = true;
  if (xTaskCreatePinnedToCore(testTask, "zte_test", kServiceTaskStack, this, 1, nullptr, 0) !=
      pdPASS) {
    testRunning_ = false;
    Serial.println("event=zte_test_failed reason=task_create");
    error = F("The test could not be started. Try again.");
    return false;
  }
  return true;
}
// #endregion METHOD_ZteService_startTest

// #region METHOD_ZteService_startSend
// PURPOSE: Keeps sends non-blocking and exclusive with polling and tests.
bool ZteService::startSend(const String& to, const String& text, String& error) {
  if (!shouldRunModule()) {
    error = F("The ZTE modem module is disabled.");
    return false;
  }
  if (sendRunning_) {
    error = F("An SMS send is already in progress.");
    return false;
  }
  if (testRunning_) {
    error = F("A connection test is in progress; try again in a few seconds.");
    return false;
  }
  if (pollCycleActive_) {
    error = F("A poll cycle is in progress; try again in a few seconds.");
    return false;
  }
  if (time(nullptr) < kEpochSynced) {
    error = F("Waiting for the internet time sync; try again in a minute.");
    return false;
  }
  sendTo_ = to;
  sendText_ = text;
  sendDone_ = false;
  sendMessage_ = "";
  sendRunning_ = true;
  if (xTaskCreatePinnedToCore(sendTask, "zte_send", kServiceTaskStack, this, 1, nullptr, 0) !=
      pdPASS) {
    sendRunning_ = false;
    Serial.println("event=zte_send_failed reason=task_create");
    error = F("The send could not be started. Try again.");
    return false;
  }
  return true;
}
// #endregion METHOD_ZteService_startSend

// #region METHOD_ZteService_syncPollTask
// PURPOSE: Aligns poll task with shouldRun (moduleEnabled gate).
void ZteService::syncPollTask(bool shouldRun) {
  if (pollHandle_ != nullptr) {
    if (!task_control::stopTask(pollHandle_, pollStopRequested_)) {
      Serial.println("event=zte_poll_stop_timeout");
      return;
    }
  }
  if (!shouldRun) {
    Serial.println("event=zte_poll_task state=stopped");
    return;
  }
  if (xTaskCreatePinnedToCore(pollTask, "zte_poll", kServiceTaskStack, this, 1, &pollHandle_, 0) !=
      pdPASS) {
    pollHandle_ = nullptr;
    Serial.println("event=zte_poll_task state=start_failed reason=task_create");
    return;
  }
  Serial.println("event=zte_poll_task state=started");
}
// #endregion METHOD_ZteService_syncPollTask

// #region METHOD_ZteService_replySnippet
// PURPOSE: Keeps diagnostic replies bounded and safe for operator messages.
String ZteService::replySnippet(const char* body) const {
  String snippet;
  for (const char* p = body; *p != '\0' && snippet.length() < 96; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    snippet += (ch >= 32 && ch <= 126) ? static_cast<char>(ch) : '.';
  }
  return snippet;
}
// #endregion METHOD_ZteService_replySnippet

// #region METHOD_ZteService_forwardSms
// PURPOSE: Makes SMTP acceptance the gate for deleting the source SMS.
bool ZteService::forwardSms(const ZteSms& sms) {
  const unsigned long startedAt = millis();
  Serial.printf("event=zte_forward_begin id=%s number=%s heap=%u\n", sms.id, sms.number,
                static_cast<unsigned>(ESP.getFreeHeap()));
  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  String subject;
  String body;
  buildZteSmsEmail(sms, stored_.label, subject, body);
  if (smtp_ == nullptr || wifi_ == nullptr) return false;
  const SmtpConfigRecord record = buildSmtpConfigRecord(smtp_->config());
  const SmtpSendResult result =
      client.sendMail(record, wifi_->mdnsHostname().c_str(), subject.c_str(), body.c_str());
  Serial.printf(
      "event=zte_forward_result id=%s result=%s stage=%s code=%d elapsed_ms=%lu heap=%u\n", sms.id,
      smtpSendResultName(result), client.failedStage(), client.lastReplyCode(),
      millis() - startedAt, static_cast<unsigned>(ESP.getFreeHeap()));
  return result == SmtpSendResult::kSuccess;
}
// #endregion METHOD_ZteService_forwardSms

// #region METHOD_ZteService_runPollCycle
// PURPOSE: Completes one at-least-once forwarding cycle without losing failed deliveries.
void ZteService::runPollCycle(ZteModem& modem) {
  // #region BLOCK_login
  Serial.printf("event=zte_poll_begin host=%s heap=%u\n", stored_.host.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
  ZteResult result = modem.login(stored_.host.c_str(), stored_.password.c_str());
  if (result != ZteResult::kSuccess) {
    publishStatus((String(F("Poll failed: ")) + modem.failedStage()).c_str());
    Serial.printf("event=zte_poll_complete result=login_failed stage=%s\n", modem.failedStage());
    return;
  }
  // #endregion BLOCK_login

  // #region BLOCK_findOldestIncoming
  ZteSms* sms = new (std::nothrow) ZteSms();
  if (sms == nullptr) {
    publishStatus("Poll skipped: out of memory.");
    Serial.println("event=zte_poll_complete result=out_of_memory");
    return;
  }
  bool found = false;
  result = modem.findOldestIncoming(*sms, found);
  if (result != ZteResult::kSuccess) {
    publishStatus((String(F("Inbox scan failed: ")) + modem.failedStage()).c_str());
    Serial.printf("event=zte_poll_complete result=scan_failed stage=%s\n", modem.failedStage());
    delete sms;
    return;
  }
  if (!found) {
    publishStatus(
        (String(F("Connected to ")) + modem.waVersion() + F("; no incoming SMS.")).c_str());
    Serial.println("event=zte_poll_complete result=inbox_empty");
    delete sms;
    return;
  }
  // #endregion BLOCK_findOldestIncoming

  // #region BLOCK_forwardAndDelete
  Serial.printf("event=zte_sms_found id=%s complete=%s\n", sms->id,
                sms->concatComplete ? "true" : "false");
  if (!forwardSms(*sms)) {
    publishStatus("Email delivery failed; SMS kept on the modem.");
    Serial.printf("event=zte_poll_complete result=forward_failed id=%s\n", sms->id);
    delete sms;
    return;
  }
  result = modem.deleteSms(*sms);
  if (result == ZteResult::kSuccess) {
    Serial.printf("event=zte_delete_complete id=%s\n", sms->id);
    publishStatus((String(F("Forwarded SMS id=")) + sms->id + F(".")).c_str());
    Serial.printf("event=zte_poll_complete result=forwarded id=%s\n", sms->id);
  } else {
    Serial.printf("event=zte_delete_failed id=%s stage=%s result=%s\n", sms->id,
                  modem.failedStage(), zteResultName(result));
    publishStatus("SMS forwarded but deletion failed; it may be sent again.");
    Serial.printf("event=zte_poll_complete result=delete_unverified id=%s\n", sms->id);
  }
  delete sms;
  // #endregion BLOCK_forwardAndDelete
}
// #endregion METHOD_ZteService_runPollCycle

// #region METHOD_ZteService_pollTask
// PURPOSE: Keeps ZTE polling alive while yielding to shared modem work.
void ZteService::pollTask(void* param) {
  auto* self = static_cast<ZteService*>(param);
  if (self != nullptr) self->runPollTask();
  vTaskDelete(nullptr);
}
// #endregion METHOD_ZteService_pollTask

void ZteService::runPollTask() {
  watchdog::addCurrentTask("zte_poll");
  char* scratch = static_cast<char*>(malloc(kZteScratchSize));
  NetworkZteChannel channel;
  ZteModem modem(channel, scratch, scratch == nullptr ? 0 : kZteScratchSize);
  bool waitingForStation = false;
  while (!pollStopRequested_) {
    if (scratch == nullptr || WiFi.status() != WL_CONNECTED) {
      if (!waitingForStation) {
        waitingForStation = true;
        Serial.printf("event=zte_poll_wait reason=%s\n",
                      scratch == nullptr ? "out_of_memory" : "sta_down");
      }
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    waitingForStation = false;
    if (testRunning_ || sendRunning_) {
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    // Only poll when forwarding enabled and smtp ready; otherwise idle.
    bool smtpReady = smtp_ != nullptr && smtp_->isLoaded() && smtp_->config().host.length() > 0 &&
                     smtp_->config().password.length() > 0;
    if (!shouldRunPoll(smtpReady)) {
      // idle wait without polling
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    pollCycleActive_ = true;
    runPollCycle(modem);
    pollCycleActive_ = false;
    const unsigned long intervalMs = pollIntervalMs();
    for (unsigned long waited = 0; waited < intervalMs && !pollStopRequested_;
         waited += kPollSliceMs) {
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
    }
  }
  free(scratch);
  Serial.println("event=zte_poll_stopped");
  watchdog::removeCurrentTask();
  pollHandle_ = nullptr;
}

// #region METHOD_ZteService_testTask
// PURPOSE: Provides the worker entry point for a non-blocking connection test.
void ZteService::testTask(void* param) {
  auto* self = static_cast<ZteService*>(param);
  if (self != nullptr) self->runTest();
  vTaskDelete(nullptr);
}
// #endregion METHOD_ZteService_testTask

void ZteService::runTest() {
  const RuntimeZteConfig candidate = testCandidate_;
  char* scratch = static_cast<char*>(malloc(kZteScratchSize));
  NetworkZteChannel channel;
  ZteModem modem(channel, scratch, scratch == nullptr ? 0 : kZteScratchSize);
  ZteResult result = modem.login(candidate.host.c_str(), candidate.password.c_str());
  String message;
  if (result == ZteResult::kSuccess) {
    ZteInboxStatus status{};
    result = modem.readInboxStatus(status);
    if (result == ZteResult::kSuccess) {
      message = String(F("Connected to ")) + modem.waVersion() + F(". Device inbox: ") +
                String(status.used) + '/' + String(status.total) + F(" messages.");
    }
  }
  free(scratch);
  if (result != ZteResult::kSuccess) {
    message = String(F("Connection failed [stage=")) + modem.failedStage() + F("]");
  }
  testMessage_ = message;
  testSuccess_ = result == ZteResult::kSuccess;
  Serial.printf("event=zte_test_complete result=%s stage=%s\n", zteResultName(result),
                modem.failedStage());
  testRunning_ = false;
  testDone_ = true;
}

// #region METHOD_ZteService_sendTask
// PURPOSE: Provides the worker entry point for a non-blocking SMS send.
void ZteService::sendTask(void* param) {
  auto* self = static_cast<ZteService*>(param);
  if (self != nullptr) self->runSend();
  vTaskDelete(nullptr);
}
// #endregion METHOD_ZteService_sendTask

void ZteService::runSend() {
  const String to = sendTo_;
  const String text = sendText_;
  const unsigned long startedAt = millis();
  Serial.printf("event=zte_send_begin to=%s units=%u epoch=%ld heap=%u\n", to.c_str(),
                static_cast<unsigned>(zteSmsUtf16Units(text.c_str())),
                static_cast<long>(time(nullptr)), static_cast<unsigned>(ESP.getFreeHeap()));

  char* scratch = static_cast<char*>(malloc(kZteScratchSize));
  NetworkZteChannel channel;
  ZteModem modem(channel, scratch, scratch == nullptr ? 0 : kZteScratchSize);
  ZteResult result = scratch == nullptr
                         ? ZteResult::kProtocolError
                         : modem.login(stored_.host.c_str(), stored_.password.c_str());
  bool confirmed = false;
  bool statusFailed = false;
  if (result == ZteResult::kSuccess) {
    result = modem.sendSms(to.c_str(), text.c_str());
  }
  if (result == ZteResult::kSuccess) {
    for (int attempt = 0; attempt < kZteSendStatusAttempts; ++attempt) {
      vTaskDelay(pdMS_TO_TICKS(kZteSendStatusDelayMs));
      ZteSendStatus status;
      const ZteResult pollResult = modem.readSendStatus(status);
      if (pollResult != ZteResult::kSuccess) {
        result = pollResult;
        break;
      }
      if (status == ZteSendStatus::kDone) {
        confirmed = true;
        break;
      }
      if (status == ZteSendStatus::kFailed) {
        statusFailed = true;
        break;
      }
    }
  }
  const String sendStage = modem.failedStage();
  const String replyDetail =
      result == ZteResult::kSendRejected ? replySnippet(modem.lastBody()) : String();
  const bool cleanupRequired = confirmed || statusFailed;
  uint16_t cleanedOutgoing = 0;
  ZteResult cleanupResult = ZteResult::kSuccess;
  String cleanupStage;
  if (cleanupRequired) {
    for (unsigned int attempt = 0; attempt < kZteOutgoingCleanupMaxAttempts; ++attempt) {
      uint16_t deletedThisAttempt = 0;
      cleanupResult = modem.cleanupOutgoing(deletedThisAttempt);
      cleanedOutgoing += deletedThisAttempt;
      cleanupStage = modem.failedStage();
      if (cleanupResult == ZteResult::kSuccess || cleanupStage != "delete_unverified" ||
          attempt + 1 == kZteOutgoingCleanupMaxAttempts) {
        break;
      }
      Serial.printf("event=zte_outgoing_cleanup_retry attempt=%u deleted=%u delay_ms=%lu\n",
                    attempt + 1, static_cast<unsigned>(deletedThisAttempt),
                    kZteOutgoingCleanupRetryDelayMs);
      vTaskDelay(pdMS_TO_TICKS(kZteOutgoingCleanupRetryDelayMs));
    }
    if (cleanupResult == ZteResult::kSuccess) {
      Serial.printf("event=zte_outgoing_cleanup_complete result=success deleted=%u\n",
                    static_cast<unsigned>(cleanedOutgoing));
    } else {
      Serial.printf("event=zte_outgoing_cleanup_complete result=%s deleted=%u stage=%s\n",
                    zteResultName(cleanupResult), static_cast<unsigned>(cleanedOutgoing),
                    cleanupStage.c_str());
    }
  }

  String message;
  if (scratch == nullptr) {
    message = F("Send failed: out of memory.");
  } else if (confirmed) {
    message = F("SMS sent to ");
    message += to;
    message += '.';
  } else if (statusFailed) {
    result = ZteResult::kProtocolError;
    message = F("The modem accepted the message but reported the send as failed ");
    message += F("(check the number and the SMS center). [stage=send_status]");
  } else if (result == ZteResult::kSuccess) {
    message = F("The modem accepted the message but its status stayed in progress; ");
    message += F("it may still be delivered. [stage=send_status]");
  } else if (result == ZteResult::kSendRejected) {
    message = F("The modem rejected the message. Modem reply: ");
    message += replyDetail;
  } else {
    message = F("Send failed [stage=");
    message += sendStage;
    message += ']';
  }
  if (cleanupRequired) {
    if (cleanupResult == ZteResult::kSuccess) {
      message += F(" Outgoing modem records cleared: ");
      message += String(cleanedOutgoing);
      message += '.';
    } else {
      message += F(" Outgoing modem records could not be cleared [stage=");
      message += cleanupStage;
      message += ']';
    }
  }
  sendMessage_ = message;
  sendSuccess_ = confirmed;
  if (!confirmed) {
    Serial.printf("event=zte_send_form form=%s\n", modem.lastSendForm());
  }
  free(scratch);
  Serial.printf(
      "event=zte_send_complete result=%s confirmed=%s stage=%s elapsed_ms=%lu detail=%s\n",
      zteResultName(result), confirmed ? "true" : "false", sendStage.c_str(), millis() - startedAt,
      replyDetail.c_str());
  sendRunning_ = false;
  sendDone_ = true;
}
