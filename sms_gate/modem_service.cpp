// #region MODULE_CONTRACT
// PURPOSE: Implements ModemService so sms_gate.ino only drives sync/load/save.
// #endregion MODULE_CONTRACT

#include "modem/modem_service.h"

#include <Arduino.h>
#include <WiFi.h>

#include "persistence/config_store_common.h"
#include "system/email_builder.h"
#include "system/modem_lock.h"
#include "modem/modem_transport.h"
#include "smtp/smtp_client.h"
#include "smtp/smtp_service.h"
#include "smtp/smtp_transport.h"
#include "system/task_control.h"
#include "system/time_sync.h"
#include "system/watchdog.h"
#include "system/wifi_manager.h"
#include "zte/zte_service.h"

namespace {

using task_control::kPollSliceMs;
using task_control::kServiceTaskStack;

constexpr size_t kModemScratchSize = 2048;

// SMTP helpers now shared via smtp_client.h:smtpSendResultName and
// smtp_service.h:logSmtpStage — local duplicates removed (P1).

}  // namespace

ModemService::ModemService() = default;

// #region METHOD_ModemService_load
// PURPOSE: Loads stored modem-source profile from NVS.
bool ModemService::load() {
  loaded_ = store_.load(stored_);
  return loaded_;
}
// #endregion METHOD_ModemService_load

// #region METHOD_ModemService_save
// PURPOSE: Persists validated candidate and updates in-memory copy.
bool ModemService::save(const RuntimeModemSourceConfig& candidate) {
  if (!store_.save(candidate)) return false;
  stored_ = candidate;
  loaded_ = true;
  return true;
}
// #endregion METHOD_ModemService_save

bool ModemService::shouldRunTask() const { return loaded_ && stored_.moduleEnabled; }

bool ModemService::shouldPoll() const { return shouldRunTask() && stored_.pollEnabled; }

bool ModemService::shouldTimeSync() const { return shouldPoll() && stored_.nitzTimeSyncEnabled; }

bool ModemService::shouldRunSms() const { return shouldPoll() && stored_.smsPollEnabled; }

// #region METHOD_ModemService_webSourceConfig
// PURPOSE: Snapshots stored profile for JSON API without credentials.
WebModemSourceConfig ModemService::webSourceConfig() const {
  WebModemSourceConfig web;
  web.present = loaded_;
  web.moduleEnabled = loaded_ ? stored_.moduleEnabled : false;
  web.pollEnabled = loaded_ ? stored_.pollEnabled : false;
  web.pollIntervalSec = loaded_ ? stored_.pollIntervalSec : kDefaultModemPollSec;
  web.label = loaded_ ? stored_.label : String();
  web.nitzTimeSyncEnabled = loaded_ ? stored_.nitzTimeSyncEnabled : true;
  web.smsPollEnabled = loaded_ ? stored_.smsPollEnabled : false;
  web.lastStatus = String();
  return web;
}
// #endregion METHOD_ModemService_webSourceConfig

// #region METHOD_ModemService_webStatus
// PURPOSE: Snapshots modem status cache for JSON API.
WebModemStatus ModemService::webStatus() const {
  const ModemStatus raw = statusCache_.read();
  WebModemStatus web;
  web.present = raw.present;
  web.cpin = String(raw.cpin);
  web.rssiDbm = raw.csqRssiDbm;
  web.ber = raw.csqBer;
  web.rsrpDbm = raw.cesqRsrpDbm;
  web.rsrqDb = raw.cesqRsrqDb;
  web.cereg = raw.ceregStat;
  web.creg = raw.cregStat;
  web.attached = raw.cgatt;
  web.oper = String(raw.copsOp);
  web.act = raw.copsAct;
  web.clock = String(raw.cclk);
  web.smsUsedMe = raw.smsUsedMe;
  web.smsTotalMe = raw.smsTotalMe;
  web.smsUsedSm = raw.smsUsedSm;
  web.smsTotalSm = raw.smsTotalSm;
  web.imei = String(raw.imei);
  web.fw = String(raw.fw);
  return web;
}
// #endregion METHOD_ModemService_webStatus

// #region METHOD_ModemService_readSourceForm
// PURPOSE: Validates modem-source form from current HTTP request.
bool ModemService::readSourceForm(WebServer& server, RuntimeModemSourceConfig& out, String& error) {
  out.moduleEnabled = server.arg("module_enabled") == F("1");
  out.pollEnabled = server.arg("poll_enabled") == F("1");
  out.smsPollEnabled = server.arg("sms_poll") == F("1");
  out.nitzTimeSyncEnabled = server.arg("nitz_time_sync") == F("1");
  // defaults when fields missing (old clients)
  if (!server.hasArg("poll_enabled") && server.hasArg("module_enabled")) {
    out.pollEnabled = out.moduleEnabled;
  }
  if (!server.hasArg("sms_poll") && server.hasArg("module_enabled")) {
    out.smsPollEnabled = out.moduleEnabled;
  }
  if (!server.hasArg("nitz_time_sync")) {
    out.nitzTimeSyncEnabled = true;
  }
  if (!parsePollInterval(server.arg("poll_interval"), out.pollIntervalSec, kMinModemPollSec,
                         kMaxModemPollSec, error)) {
    return false;
  }
  out.label = server.arg("label");
  out.label.trim();
  if (out.label.length() > kMaxModemLabelLength || !isPrintableAscii(out.label)) {
    error = F("The phone number or alias must contain up to 31 printable ASCII characters.");
    return false;
  }
  // enforce dependencies: sms and nitz require poll && module
  if ((out.smsPollEnabled || out.nitzTimeSyncEnabled) && (!out.pollEnabled || !out.moduleEnabled)) {
    // allow but ineffective; don't reject to keep UI flexible
  }
  return true;
}
// #endregion METHOD_ModemService_readSourceForm

// #region METHOD_ModemService_pollIntervalMs
// PURPOSE: Returns NVS-backed poll interval as milliseconds.
unsigned long ModemService::pollIntervalMs() const {
  const uint16_t sec = loaded_ ? stored_.pollIntervalSec : kDefaultModemPollSec;
  if (!isValidModemPollInterval(sec)) return kDefaultModemPollSec * 1000UL;
  return static_cast<unsigned long>(sec) * 1000UL;
}
// #endregion METHOD_ModemService_pollIntervalMs

// #region METHOD_ModemService_publishStatus
// PURPOSE: Atomically publishes one polled snapshot behind portMUX.
void ModemService::publishStatus(const ModemStatus& status) { statusCache_.publish(status); }
// #endregion METHOD_ModemService_publishStatus

// #region METHOD_ModemService_readStatus
// PURPOSE: Snapshots status cache for JSON API.
ModemStatus ModemService::readStatus() const { return statusCache_.read(); }
// #endregion METHOD_ModemService_readStatus

// #region METHOD_ModemService_sendStatus
// PURPOSE: Snapshots async send progress for polling UI.
WebAsyncOp ModemService::sendStatus() const {
  WebAsyncOp op;
  op.running = sendRunning_;
  op.done = sendDone_;
  if (sendDone_) {
    op.result = sendSuccess_ ? "success" : "failed";
    op.message = sendMessage_;
  }
  return op;
}
// #endregion METHOD_ModemService_sendStatus

// #region METHOD_ModemService_shouldRunSms
// PURPOSE: Returns whether poll should forward given SMTP, Wi-Fi and SIM state.
bool ModemService::shouldRunSms(const ModemStatus& snapshot) const {
  if (!shouldRunSms()) return false;
  if (smtp_ == nullptr || !smtp_->isLoaded() || smtp_->config().host.length() == 0 ||
      smtp_->config().password.length() == 0)
    return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!snapshot.present || strcmp(snapshot.cpin, "READY") != 0) return false;
  if (sendRunning_) return false;
  return true;
}
// #endregion METHOD_ModemService_shouldRunSms

// #region METHOD_ModemService_forwardSms
// PURPOSE: Forwards one ModemSms via SMTP and logs stage/reply for tracing.
bool ModemService::forwardSms(const ModemSms& sms) {
  if (smtp_ == nullptr || wifi_ == nullptr) return false;
  const unsigned long startedAt = millis();
  Serial.printf("event=modem_forward_begin id=%s number=%s heap=%u\n", sms.id, sms.number,
                static_cast<unsigned>(ESP.getFreeHeap()));
  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  String subject;
  String body;
  ::buildModemSmsEmail(sms, stored_.label, subject, body);
  const SmtpConfigRecord record = buildSmtpConfigRecord(smtp_->config());
  const SmtpSendResult result =
      client.sendMail(record, wifi_->mdnsHostname().c_str(), subject.c_str(), body.c_str());
  Serial.printf(
      "event=modem_forward_result id=%s result=%s stage=%s code=%d elapsed_ms=%lu heap=%u\n",
      sms.id, smtpSendResultName(result), client.failedStage(), client.lastReplyCode(),
      millis() - startedAt, static_cast<unsigned>(ESP.getFreeHeap()));
  return result == SmtpSendResult::kSuccess;
}
// #endregion METHOD_ModemService_forwardSms

// #region METHOD_ModemService_runPollCycle
// PURPOSE: Finds oldest unread SMS, forwards via SMTP and deletes on 250 OK.
void ModemService::runPollCycle(ModemClient& client) {
  // #region BLOCK_scanInbox
  Serial.printf("event=modem_poll_begin heap=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  pollCycleActive_ = true;
  ModemSms sms;
  bool found = false;
  ModemResult result = client.findOldestUnread(sms, found);
  if (result != ModemResult::kSuccess) {
    String safeStage = String(client.failedStage());
    safeStage.replace("=", "_");
    Serial.printf("event=modem_poll_complete result=scan_failed stage=%s\n", safeStage.c_str());
    pollCycleActive_ = false;
    return;
  }
  if (!found) {
    Serial.println("event=modem_poll_complete result=inbox_empty");
    pollCycleActive_ = false;
    return;
  }
  // #endregion BLOCK_scanInbox

  // #region BLOCK_forwardAndDelete
  Serial.printf("event=modem_sms_found id=%s number=%s complete=%s\n", sms.id, sms.number,
                sms.concatComplete ? "true" : "false");
  if (!forwardSms(sms)) {
    Serial.printf("event=modem_poll_complete result=forward_failed id=%s\n", sms.id);
    pollCycleActive_ = false;
    return;
  }
  result = client.deleteSms(sms.id);
  if (result == ModemResult::kSuccess) {
    Serial.printf("event=modem_delete_complete id=%s\n", sms.id);
    Serial.printf("event=modem_poll_complete result=forwarded id=%s\n", sms.id);
  } else {
    String safeStage = String(client.failedStage());
    safeStage.replace("=", "_");
    Serial.printf("event=modem_delete_failed id=%s stage=%s\n", sms.id, safeStage.c_str());
    Serial.printf("event=modem_poll_complete result=delete_unverified id=%s\n", sms.id);
  }
  pollCycleActive_ = false;
  // #endregion BLOCK_forwardAndDelete
}
// #endregion METHOD_ModemService_runPollCycle

// #region METHOD_ModemService_runPollTask
// PURPOSE: Modem task: init when moduleEnabled, then poll status and SMS when pollEnabled.
void ModemService::runPollTask() {
  watchdog::addCurrentTask("modem_poll");
  char* scratch = static_cast<char*>(malloc(kModemScratchSize));
  if (scratch == nullptr) {
    Serial.println("event=modem_error stage=no_scratch");
    watchdog::removeCurrentTask();
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  // Init must be serialized with GNSS task — hold the shared Serial1 lock.
  modem_lock::take(15000);
  ModemTransport transport;
  transport.begin();
  transport.powerPulse();
  Serial.println("event=modem_init_begin variant=classic");
  vTaskDelay(pdMS_TO_TICKS(3000));
  ModemClient client(transport, scratch, kModemScratchSize);
  ModemResult initResult = client.init();
  modem_lock::give();
  if (initResult == ModemResult::kSuccess) {
    Serial.printf("event=modem_ready variant=classic heap=%u stack_hwm=%u\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  } else {
    Serial.printf("event=modem_error stage=%s stack_hwm=%u\n", client.failedStage(),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  while (!taskStopRequested_) {
    if (shouldPoll()) {
      // Serialize Serial1 with GNSS task — GNSS poll can hold 10-12s.
      bool haveLock = modem_lock::take(15000);
      ModemStatus status;
      ModemResult result = ModemResult::kTimeout;
      if (!haveLock) {
        Serial.println("event=modem_poll_skipped reason=modem_busy");
        status.present = false;
        String safeStage = "modem_busy";
        publishStatus(status);
        Serial.printf("event=modem_error stage=%s\n", safeStage.c_str());
      } else {
        result = client.pollStatus(status);
        if (result == ModemResult::kSuccess) {
          status.updatedMs = millis();
          publishStatus(status);
          Serial.printf(
              "event=modem_status present=%s cpin=%s cereg=%d creg=%d rssi=%d rsrp=%d rsrq=%d "
              "cops=%s "
              "act=%d used_me=%u total_me=%u used_sm=%u total_sm=%u cclk=%s stack_hwm=%u\n",
              status.present ? "true" : "false", status.cpin, status.ceregStat, status.cregStat,
              status.csqRssiDbm, status.cesqRsrpDbm, status.cesqRsrqDb, status.copsOp,
              status.copsAct, static_cast<unsigned>(status.smsUsedMe),
              static_cast<unsigned>(status.smsTotalMe), static_cast<unsigned>(status.smsUsedSm),
              static_cast<unsigned>(status.smsTotalSm), status.cclk,
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
          if (shouldTimeSync() && timeSync_ != nullptr && status.cclk[0] != '\0') {
            int64_t epochMs = 0;
            if (cclkToEpochMs(status.cclk, epochMs)) {
              timeSync_->feedNitzSample(epochMs, 1500);
              Serial.printf("event=nitz_time_feed cclk=%s epoch_ms=%lld\n", status.cclk,
                            (long long)epochMs);
            }
          }
        } else {
          ModemStatus absent;
          absent.present = false;
          String safeStage = String(client.failedStage());
          safeStage.replace("=", "_");
          publishStatus(absent);
          Serial.printf("event=modem_error stage=%s\n", safeStage.c_str());
        }
        if (!sendRunning_) {
          const ModemStatus snapshot = readStatus();
          if (shouldRunSms(snapshot)) {
            runPollCycle(client);
          }
        }
        modem_lock::give();
      }
    } else {
      // Module disabled or poll disabled: don't touch Serial1.
      if (!shouldRunTask()) {
        // If module disabled, task should have been stopped; but stay idle if still running.
      }
    }
    const unsigned long intervalMs = pollIntervalMs();
    for (unsigned long waited = 0; waited < intervalMs && !taskStopRequested_;
         waited += kPollSliceMs) {
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
    }
  }
  transport.end();
  free(scratch);
  Serial.println("event=modem_task_stopped");
  watchdog::removeCurrentTask();
  taskHandle_ = nullptr;
  vTaskDelete(nullptr);
}
// #endregion METHOD_ModemService_runPollTask

// #region METHOD_ModemService_pollTask
// PURPOSE: FreeRTOS entry for poll task.
void ModemService::pollTask(void* param) {
  auto* self = static_cast<ModemService*>(param);
  if (self != nullptr) self->runPollTask();
}
// #endregion METHOD_ModemService_pollTask

// #region METHOD_ModemService_syncTask
// PURPOSE: Ensures modem poll task runs only when moduleEnabled.
void ModemService::syncTask() {
  if (taskHandle_ != nullptr) {
    if (!task_control::stopTask(taskHandle_, taskStopRequested_)) {
      Serial.println("event=modem_task_stop_timeout");
      return;
    }
  }
  if (!shouldRunTask()) {
    Serial.println("event=modem_task_stopped reason=module_disabled");
    return;
  }
  if (xTaskCreatePinnedToCore(pollTask, "modem_poll", kServiceTaskStack, this, 1, &taskHandle_,
                              0) != pdPASS) {
    taskHandle_ = nullptr;
    Serial.println("event=modem_task_start_failed reason=task_create");
    return;
  }
  Serial.println("event=modem_task_started");
}
// #endregion METHOD_ModemService_syncTask

// #region METHOD_ModemService_stopTask
// PURPOSE: Stops poll task for exclusive Serial1 send.
bool ModemService::stopTask() {
  if (taskHandle_ == nullptr) return true;
  if (!task_control::stopTask(taskHandle_, taskStopRequested_)) {
    Serial.println("event=modem_task_stop_timeout");
    return false;
  }
  return true;
}
// #endregion METHOD_ModemService_stopTask

// #region METHOD_ModemService_startSend
// PURPOSE: Starts one AT send; fails when busy or SIM not ready.
bool ModemService::startSend(const String& to, const String& text, String& error) {
  if (!shouldRunTask()) {
    error = F("The internal modem module is disabled.");
    return false;
  }
  if (sendRunning_) {
    error = F("An SMS send is already in progress.");
    return false;
  }
  if (zte_ != nullptr && zte_->isSendRunning()) {
    error = F("An SMS send is already in progress.");
    return false;
  }
  if (zte_ != nullptr && zte_->isPollCycleActive()) {
    error = F("A poll cycle is in progress; try again in a few seconds.");
    return false;
  }
  if (pollCycleActive_) {
    error = F("A modem poll cycle is in progress; try again in a few seconds.");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    error = F("The device is not connected to a Wi-Fi network.");
    return false;
  }
  const ModemStatus snapshot = readStatus();
  if (!snapshot.present) {
    error = F("The internal modem is not responding; try again.");
    return false;
  }
  if (strcmp(snapshot.cpin, "READY") != 0) {
    error = F("The SIM card is not ready; check the modem status.");
    return false;
  }
  const size_t units = smsUtf16Units(text.c_str());
  if (units == kSmsInvalidUnits) {
    error = F("The message is not valid UTF-8 text.");
    return false;
  }
  if (!isValidSmsRecipient(to)) {
    error = F("Recipient must be 3\u201320 digits with an optional leading +.");
    return false;
  }
  sendTo_ = to;
  sendText_ = text;
  sendDone_ = false;
  sendMessage_ = "";
  sendSuccess_ = false;
  sendRunning_ = true;
  if (xTaskCreatePinnedToCore(sendTask, "modem_send", kServiceTaskStack, this, 1, nullptr, 0) !=
      pdPASS) {
    sendRunning_ = false;
    Serial.println("event=modem_send_failed reason=task_create");
    error = F("The send could not be started. Try again.");
    return false;
  }
  return true;
}
// #endregion METHOD_ModemService_startSend

// #region METHOD_ModemService_sendTask
// PURPOSE: FreeRTOS entry for send task.
void ModemService::sendTask(void* param) {
  auto* self = static_cast<ModemService*>(param);
  if (self != nullptr) self->runSend();
  vTaskDelete(nullptr);
}
// #endregion METHOD_ModemService_sendTask

// #region METHOD_ModemService_runSend
// PURPOSE: Executes blocking AT send on its own task.
void ModemService::runSend() {
  const String to = sendTo_;
  const String text = sendText_;
  const unsigned long startedAt = millis();
  Serial.printf("event=modem_send_begin to=%s units=%u heap=%u\n", to.c_str(),
                static_cast<unsigned>(smsUtf16Units(text.c_str())),
                static_cast<unsigned>(ESP.getFreeHeap()));
  char* scratch = static_cast<char*>(malloc(kModemScratchSize));
  const bool stopped = stopTask();
  if (!stopped) {
    Serial.println("event=modem_send_complete result=stop_failed stage=stop_timeout");
    free(scratch);
    sendMessage_ = F("Send failed: modem busy, try again.");
    sendSuccess_ = false;
    sendRunning_ = false;
    sendDone_ = true;
    vTaskDelete(nullptr);
    return;
  }
  modem_lock::take(8000);
  ModemTransport transport;
  transport.begin();
  ModemClient client(transport, scratch, scratch == nullptr ? 0 : kModemScratchSize);
  ModemResult result =
      scratch == nullptr ? ModemResult::kProtocolError : client.sendSms(to.c_str(), text.c_str());
  String message;
  if (scratch == nullptr) {
    message = F("Send failed: out of memory.");
  } else if (result == ModemResult::kSuccess) {
    message = F("SMS sent to ");
    message += to;
    message += '.';
  } else if (result == ModemResult::kSendRejected) {
    message = F("The modem rejected the message [stage=");
    message += client.failedStage();
    message += ']';
    if (client.lastReply()[0] != '\0') {
      message += F(" Modem reply: ");
      message += client.lastReply();
    }
  } else if (result == ModemResult::kTimeout) {
    message = F("Send timed out [stage=");
    message += client.failedStage();
    message += ']';
  } else {
    message = F("Send failed [stage=");
    message += client.failedStage();
    message += ']';
  }
  sendMessage_ = message;
  sendSuccess_ = result == ModemResult::kSuccess;
  if (!sendSuccess_) {
    Serial.printf("event=modem_send_form form=%s\n", client.lastSendHex());
  }
  free(scratch);
  transport.end();
  modem_lock::give();
  syncTask();
  Serial.printf("event=modem_send_complete result=%s stage=%s elapsed_ms=%lu heap=%u\n",
                result == ModemResult::kSuccess
                    ? "success"
                    : (result == ModemResult::kSendRejected
                           ? "send_rejected"
                           : (result == ModemResult::kTimeout ? "timeout" : "protocol_error")),
                client.failedStage(), millis() - startedAt,
                static_cast<unsigned>(ESP.getFreeHeap()));
  sendRunning_ = false;
  sendDone_ = true;
}
// #endregion METHOD_ModemService_runSend
