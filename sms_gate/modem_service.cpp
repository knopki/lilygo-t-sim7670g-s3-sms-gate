// #region MODULE_CONTRACT
// PURPOSE: Keeps SIM7670G polling and forwarding out of the firmware shell.
// SCOPE:
// - Owns modem profile loading, task gating, polling, SMS forwarding, and web-state projection.
// - NOT: AT dialog parsing, serial transport ownership, SMTP delivery, and HTTP route handling.
// INVARIANTS:
// - Volatile concat state marks SMTP acceptance before CMGD and retries only pending
//   cleanup in the same boot;
// - non-cacheable concat parts are forwarded once as explicit incomplete messages
//   without blocking CMGL.
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
// PURPOSE: Restores the modem profile from NVS so enable/poll choices
// survive reboot without re-provisioning.
bool ModemService::load() {
  loaded_ = store_.load(stored_);
  return loaded_;
}
// #endregion METHOD_ModemService_load

// #region METHOD_ModemService_save
// PURPOSE: Commits the validated candidate and mirrors it in memory, so
// the running task and the next boot see the same profile.
bool ModemService::save(const RuntimeModemSourceConfig& candidate) {
  if (!store_.save(candidate)) return false;
  stored_ = candidate;
  loaded_ = true;
  return true;
}
// #endregion METHOD_ModemService_save

// #region METHOD_ModemService_shouldRunTask
// PURPOSE: Gates task creation on a loaded, enabled modem profile.
bool ModemService::shouldRunTask() const { return loaded_ && stored_.moduleEnabled; }
// #endregion METHOD_ModemService_shouldRunTask

// #region METHOD_ModemService_shouldPoll
// PURPOSE: Gates polling on the profile's module and poll switches.
bool ModemService::shouldPoll() const { return shouldRunTask() && stored_.pollEnabled; }
// #endregion METHOD_ModemService_shouldPoll

// #region METHOD_ModemService_shouldTimeSync
// PURPOSE: Gates NITZ feeding on an active modem poll profile.
bool ModemService::shouldTimeSync() const { return shouldPoll() && stored_.nitzTimeSyncEnabled; }
// #endregion METHOD_ModemService_shouldTimeSync

// #region METHOD_ModemService_shouldRunSms
// PURPOSE: Gates SMS polling on the profile's SMS switch.
bool ModemService::shouldRunSms() const { return shouldPoll() && stored_.smsPollEnabled; }
// #endregion METHOD_ModemService_shouldRunSms

// #region METHOD_ModemService_webSourceConfig
// PURPOSE: Projects the stored profile into UI fields, so the JSON API
// renders settings without ever exposing credentials.
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
// PURPOSE: Projects the portMUX status cache into UI fields, so HTTP
// reads never touch Serial1 or block the poll task.
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
// PURPOSE: Screens the HTTP form before save(), so invalid intervals,
// oversized labels and non-printable input are rejected at the boundary
// instead of becoming the boot profile.
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
// PURPOSE: Falls back to the default interval when the record is missing
// or out of range, so a corrupt value cannot produce a broken poll delay.
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
// PURPOSE: Copies the status cache consistently, so send gating and API
// reads see one snapshot instead of torn state.
ModemStatus ModemService::readStatus() const { return statusCache_.read(); }
// #endregion METHOD_ModemService_readStatus

// #region METHOD_ModemService_sendStatus
// PURPOSE: Exposes async send progress as plain data, so the UI polls
// completion without blocking or joining the send task.
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

// #region METHOD_ModemService_shouldRunSmsForSnapshot
// PURPOSE: Gates forwarding on SMTP credentials, Wi-Fi and SIM READY, so
// the poll cycle never starts a delivery it cannot complete.
bool ModemService::shouldRunSms(const ModemStatus& snapshot) const {
  if (!shouldRunSms()) return false;
  const SmtpConfigRecord smtpConfig = smtp_ != nullptr ? smtp_->configRecord() : SmtpConfigRecord{};
  if (smtp_ == nullptr || !smtp_->isLoaded() || smtpConfig.host[0] == '\0' ||
      smtpConfig.password[0] == '\0')
    return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!snapshot.present || strcmp(snapshot.cpin, "READY") != 0) return false;
  if (sendRunning_) return false;
  return true;
}
// #endregion METHOD_ModemService_shouldRunSmsForSnapshot

// #region METHOD_ModemService_forwardSms
// PURPOSE: Delivers one SMS through the shared SMTP path, so the delete
// step can rely on the same 250-OK acceptance contract as the ZTE source;
// event fields stay on the safe whitelist.
bool ModemService::forwardSms(const ModemSms& sms) {
  if (smtp_ == nullptr || wifi_ == nullptr) return false;
  const unsigned long startedAt = millis();
  Serial.printf("event=modem_forward_begin id=%s heap=%u\n", sms.id,
                static_cast<unsigned>(ESP.getFreeHeap()));
  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  String subject;
  String body;
  ::buildModemSmsEmail(sms, stored_.label, subject, body);
  const SmtpConfigRecord record = smtp_->configRecord();
  const SmtpSendResult result =
      client.sendMail(record, wifi_->mdnsHostname().c_str(), subject.c_str(), body.c_str());
  Serial.printf(
      "event=modem_forward_result id=%s result=%s stage=%s code=%d elapsed_ms=%lu heap=%u\n",
      sms.id, smtpSendResultName(result), client.failedStage(), client.lastReplyCode(),
      millis() - startedAt, static_cast<unsigned>(ESP.getFreeHeap()));
  return result == SmtpSendResult::kSuccess;
}
// #endregion METHOD_ModemService_forwardSms

// #region METHOD_ModemService_deleteConcatSet
// PURPOSE: Retries only CMGD operations still pending after SMTP acceptance,
// selecting each part's original storage so ME/SM fallback cannot delete an
// identically indexed unrelated SMS or resend an accepted email.
bool ModemService::deleteConcatSet(ModemClient& client, size_t setIndex) {
  const uint8_t total = concatCache_.total(setIndex);
  for (size_t partIndex = 0; partIndex < total; ++partIndex) {
    if (!concatCache_.partNeedsDelete(setIndex, partIndex)) continue;
    const ModemSms* part = concatCache_.part(setIndex, partIndex);
    if (part == nullptr || client.selectReadStorage(part->storage) != ModemResult::kSuccess ||
        client.deleteSms(part->id) != ModemResult::kSuccess) {
      const char* id = part != nullptr ? part->id : "unknown";
      String safeStage = String(client.failedStage());
      safeStage.replace("=", "_");
      Serial.printf("event=modem_concat_cleanup_pending id=%s stage=%s\n", id, safeStage.c_str());
      return false;
    }
    if (!concatCache_.markPartDeleted(setIndex, partIndex)) return false;
    Serial.printf("event=modem_concat_delete_complete id=%s\n", part->id);
  }
  return true;
}
// #endregion METHOD_ModemService_deleteConcatSet

// #region METHOD_ModemService_forwardCompleteConcat
// PURPOSE: Sends a complete cached set at most once per boot after SMTP 250,
// then retries only unfinished CMGD operations until every source record is
// deleted; a reboot may still repeat delivery under the at-least-once model.
bool ModemService::forwardCompleteConcat(ModemClient& client, size_t setIndex) {
  const uint8_t total = concatCache_.total(setIndex);
  if (total < 2) return false;
  if (concatCache_.completeReadyForSmtp(setIndex)) {
    ModemSms joined;
    if (!concatCache_.buildComplete(setIndex, joined)) return false;
    Serial.printf("event=modem_concat_complete total=%u id=%s\n", static_cast<unsigned>(total),
                  joined.id);
    if (!forwardSms(joined) || !concatCache_.markCompleteSmtpAccepted(setIndex)) return false;
  }
  if (!deleteConcatSet(client, setIndex)) return false;
  if (concatCache_.removable(setIndex)) concatCache_.remove(setIndex);
  return true;
}
// #endregion METHOD_ModemService_forwardCompleteConcat

// #region METHOD_ModemService_forwardExpiredConcat
// PURPOSE: Forwards each available expired fragment at most once per boot
// with the shared INCOMPLETE marker, marking SMTP acceptance before its CMGD
// so later polls retry only deletion; missing parts stay explicit.
bool ModemService::forwardExpiredConcat(ModemClient& client, size_t setIndex) {
  const uint8_t total = concatCache_.total(setIndex);
  for (size_t partIndex = 0; partIndex < total; ++partIndex) {
    const ModemSms* cached = concatCache_.part(setIndex, partIndex);
    if (cached == nullptr) continue;
    if (concatCache_.partReadyForSmtp(setIndex, partIndex)) {
      ModemSms fragment = *cached;
      fragment.concatComplete = false;
      snprintf(fragment.concatReceived, sizeof(fragment.concatReceived), "%u",
               static_cast<unsigned>(partIndex + 1));
      snprintf(fragment.concatTotal, sizeof(fragment.concatTotal), "%u",
               static_cast<unsigned>(total));
      Serial.printf("event=modem_concat_expired seq=%u total=%u id=%s\n",
                    static_cast<unsigned>(partIndex + 1), static_cast<unsigned>(total),
                    fragment.id);
      if (!forwardSms(fragment) || !concatCache_.markPartSmtpAccepted(setIndex, partIndex))
        return false;
    }
    if (!deleteConcatSet(client, setIndex)) return false;
  }
  if (concatCache_.removable(setIndex)) concatCache_.remove(setIndex);
  return true;
}
// #endregion METHOD_ModemService_forwardExpiredConcat

// #region METHOD_ModemService_runPollCycle
// PURPOSE: Delivers stored SMS at-least-once — an SMS is deleted only after
// SMTP acceptance — and leaves the modem recoverable after every outcome:
// outcome logged with a stable result token, read storage back to ME,
// pollCycleActive_ cleared exactly once, so one failed cycle can neither
// stop forwarding nor misroute new incoming SMS to SM.
void ModemService::runPollCycle(ModemClient& client) {
  // #region BLOCK_scanInbox
  Serial.printf("event=modem_poll_begin heap=%u stack_hwm=%u\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  pollCycleActive_ = true;
  concatCache_.advanceCycle();
  // A cache can retain ten IDs (two sets × five parts); one additional
  // candidate lets this cycle forward a non-concat record. Keeping this
  // bounded array small preserves the modem_poll task stack for CMGR/PDU
  // parsing buffers.
  constexpr size_t kCandidateCapacity =
      ModemConcatCache::kMaxSets * ModemConcatCache::kMaxParts + 1;
  ModemInboxCandidate candidates[kCandidateCapacity];
  size_t candidateCount = 0;
  const char* activeStorage = "ME";
  bool scanOk = client.selectReadStorage(activeStorage) == ModemResult::kSuccess;
  if (scanOk)
    scanOk = client.findUnreadCandidates(candidates, kCandidateCapacity, candidateCount) ==
             ModemResult::kSuccess;
  if (scanOk && candidateCount == 0) {
    // SM remains a read/delete-only fallback. Scan a list so already-cached
    // parts cannot repeatedly block their later siblings.
    activeStorage = "SM";
    scanOk = client.selectReadStorage(activeStorage) == ModemResult::kSuccess &&
             client.findUnreadCandidates(candidates, kCandidateCapacity, candidateCount) ==
                 ModemResult::kSuccess;
  }
  ModemSms sms;
  bool found = false;
  bool fallbackFound = false;
  bool concatPending = false;
  if (scanOk) {
    for (size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
      if (concatCache_.containsId(candidates[candidateIndex].id, activeStorage)) {
        concatPending = true;
        continue;
      }
      // Reuse the forwarding object while scanning. The cache copies concat
      // parts, so retaining a separate candidate would consume another large
      // ModemSms frame alongside CMGR/PDU parsing buffers.
      if (client.readSms(candidates[candidateIndex].id, sms) != ModemResult::kSuccess) {
        scanOk = false;
        break;
      }
      snprintf(sms.storage, sizeof(sms.storage), "%s", activeStorage);
      ModemConcatInfo concat;
      if (client.probeConcat(sms.id, concat) != ModemResult::kSuccess) {
        // CMGR already marked this record read. A malformed sender-controlled
        // UDH must not stop this bounded candidate scan and starve newer SMS.
        String safeStage = String(client.failedStage());
        safeStage.replace("=", "_");
        Serial.printf("event=modem_concat_probe_skipped id=%s stage=%s\n", sms.id,
                      safeStage.c_str());
        continue;
      }
      if (!concat.present) {
        found = true;
        break;
      }
      if (!concatCache_.store(sms, concat)) {
        const char* reason = concat.total > kMaxSmsMultipartParts ? "total_over_limit" : "capacity";
        // An unbounded set cannot stay at the CMGL head. Preserve the source
        // record until the existing incomplete forwarding path gets SMTP 250,
        // then stop this cycle after its one delivery operation.
        sms.concatComplete = false;
        snprintf(sms.concatReceived, sizeof(sms.concatReceived), "%u",
                 static_cast<unsigned>(concat.seq));
        snprintf(sms.concatTotal, sizeof(sms.concatTotal), "%u",
                 static_cast<unsigned>(concat.total));
        found = true;
        fallbackFound = true;
        Serial.printf(
            "event=modem_concat_fallback reason=%s ref=%u ref_width=%u seq=%u total=%u id=%s\n",
            reason, static_cast<unsigned>(concat.ref), concat.refIs16Bit ? 16U : 8U,
            static_cast<unsigned>(concat.seq), static_cast<unsigned>(concat.total), sms.id);
        break;
      }
      concatPending = true;
      Serial.printf("event=modem_concat_cached ref=%u ref_width=%u seq=%u total=%u id=%s\n",
                    static_cast<unsigned>(concat.ref), concat.refIs16Bit ? 16U : 8U,
                    static_cast<unsigned>(concat.seq), static_cast<unsigned>(concat.total), sms.id);
    }
  }
  const char* scanStage = client.failedStage();
  // #endregion BLOCK_scanInbox

  // #region BLOCK_forwardAndDelete
  bool concatHandled = false;
  size_t concatSet = 0;
  if (scanOk && !fallbackFound && concatCache_.findComplete(concatSet)) {
    concatHandled = true;
    if (forwardCompleteConcat(client, concatSet))
      Serial.println("event=modem_poll_complete result=concat_forwarded");
    else
      Serial.println("event=modem_poll_complete result=concat_forward_failed");
  } else if (scanOk && !fallbackFound && concatCache_.findExpired(concatSet)) {
    concatHandled = true;
    if (forwardExpiredConcat(client, concatSet))
      Serial.println("event=modem_poll_complete result=concat_incomplete_forwarded");
    else
      Serial.println("event=modem_poll_complete result=concat_incomplete_failed");
  } else if (scanOk && found) {
    // Safe fields only — never the sender number, date, body or its
    // reversible UCS2 hex (review finding: OTP/links landed in Serial).
    Serial.printf("event=modem_sms_found id=%s complete=%s text_len=%u text_hex_len=%u\n", sms.id,
                  sms.concatComplete ? "true" : "false", static_cast<unsigned>(strlen(sms.text)),
                  static_cast<unsigned>(strlen(sms.text) * 2));
    if (!forwardSms(sms)) {
      Serial.printf("event=modem_poll_complete result=forward_failed id=%s\n", sms.id);
    } else {
      // Delete while the SMS storage is still selected so CMGD=<idx> applies
      // to the correct mem1; the ME restore happens in the cleanup tail.
      const ModemResult deleteResult = client.deleteSms(sms.id);
      if (deleteResult == ModemResult::kSuccess) {
        Serial.printf("event=modem_delete_complete id=%s\n", sms.id);
        Serial.printf("event=modem_poll_complete result=forwarded id=%s\n", sms.id);
      } else {
        String safeStage = String(client.failedStage());
        safeStage.replace("=", "_");
        Serial.printf("event=modem_delete_failed id=%s stage=%s\n", sms.id, safeStage.c_str());
        Serial.printf("event=modem_poll_complete result=delete_unverified id=%s\n", sms.id);
      }
    }
  }
  // #endregion BLOCK_forwardAndDelete

  // #region BLOCK_restoreReadStorage
  // PURPOSE: report the scan outcome before the restore attempt,
  // so a failed ME restore never masks the original scan/forward failure.
  if (!scanOk) {
    String safeStage = String(scanStage);
    safeStage.replace("=", "_");
    Serial.printf("event=modem_poll_complete result=scan_failed stage=%s\n", safeStage.c_str());
  } else if (!found && !concatHandled) {
    Serial.println(concatPending ? "event=modem_poll_complete result=concat_pending"
                                 : "event=modem_poll_complete result=inbox_empty");
  }
  if (client.selectReadStorage("ME") != ModemResult::kSuccess) {
    Serial.println("event=modem_storage_restore_failed stage=cpms");
  }
  pollCycleActive_ = false;
  // #endregion BLOCK_restoreReadStorage
}
// #endregion METHOD_ModemService_runPollCycle

// #region METHOD_ModemService_runPollTask
// PURPOSE: Owns the modem's Serial1 session for its whole lifetime —
// init, status poll, NITZ feed and SMS cycles in one serialized loop — so
// status, time and SMS never race each other or GNSS on the shared port.
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
  ModemTransport transport;
  ModemClient client(transport, scratch, kModemScratchSize);
  ModemResult initResult = ModemResult::kTimeout;
  bool initComplete = false;
  // Init must be serialized with GNSS task — never touch Serial1 without the shared lock.
  while (!taskStopRequested_ && !initComplete) {
    modem_lock::ScopedModemLock initLock(15000);
    if (!initLock.held()) {
      Serial.println("event=modem_init_skipped reason=modem_busy");
    } else {
      transport.begin();
      transport.powerPulse();
      Serial.println("event=modem_init_begin variant=classic");
      vTaskDelay(pdMS_TO_TICKS(3000));
      initResult = client.init();
      initComplete = true;
    }
    if (!initComplete && !taskStopRequested_) {
      watchdog::reset();
      vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
    }
  }
  if (!initComplete) {
    transport.end();
    free(scratch);
    Serial.println("event=modem_task_stopped");
    watchdog::removeCurrentTask();
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
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
          if (shouldTimeSync() && timeSync_ != nullptr && isModemNetworkRegistered(status) &&
              status.cclk[0] != '\0') {
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
// PURPOSE: Ensures modem poll task runs only when its profile permits it outside safe mode.
void ModemService::syncTask() {
  if (taskHandle_ != nullptr) {
    if (!task_control::stopTask(taskHandle_, taskStopRequested_)) {
      Serial.println("event=modem_task_stop_timeout");
      return;
    }
  }
  if (watchdog::isSafeMode()) {
    Serial.println("event=modem_task_stopped reason=safe_mode");
    return;
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
// PURPOSE: Gates the async send at the HTTP boundary, so concurrent or
// SIM-unready sends fail fast instead of colliding on the shared Serial1.
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
// PURPOSE: Runs the blocking AT send on its own task so a hung modem or busy
// mutex cannot strand the shared poll task: every outcome releases transport
// and lock exactly once, publishes final send state before the poll task
// restarts, and restarts poll only after a clean stop.
void ModemService::runSend() {
  const String to = sendTo_;
  const String text = sendText_;
  const unsigned long startedAt = millis();
  // No recipient here: it is an HTTP form value, never logged.
  Serial.printf("event=modem_send_begin units=%u heap=%u\n",
                static_cast<unsigned>(smsUtf16Units(text.c_str())),
                static_cast<unsigned>(ESP.getFreeHeap()));
  // Per-outcome flags so the single cleanup tail restores each shared
  // resource (transport, mutex, poll lifecycle) at most once.
  bool pollStopped = false;
  bool lockHeld = false;
  bool transportStarted = false;
  // Function scope so the cleanup tail can end it; begin/end are guarded
  // and the constructor has no side effects.
  ModemTransport transport;
  char* scratch = static_cast<char*>(malloc(kModemScratchSize));
  ModemResult result = ModemResult::kProtocolError;
  const char* stage = "no_scratch";
  const char* resultToken = "protocol_error";
  String message;

  // #region BLOCK_attemptAtSend
  if (scratch == nullptr) {
    // OOM: keep the healthy poll task running; publish failure only.
    message = F("Send failed: out of memory.");
  } else {
    pollStopped = stopTask();
    if (!pollStopped) {
      // The old poll task may still run — a duplicate via syncTask() would
      // race it on the shared Serial1, so the send is abandoned instead.
      stage = "stop_timeout";
      resultToken = "stop_failed";
      message = F("Send failed: modem busy, try again.");
    } else {
      lockHeld = modem_lock::take(12000);
      if (!lockHeld) {
        stage = "modem_busy";
        resultToken = "lock_failed";
        message = F("Send failed: modem busy, try again.");
      } else {
        transport.begin();
        transportStarted = true;
        ModemClient client(transport, scratch, kModemScratchSize);
        result = client.sendSms(to.c_str(), text.c_str());
        stage = client.failedStage();
        if (result == ModemResult::kSuccess) {
          message = F("SMS sent to ");
          message += to;
          message += '.';
        } else if (result == ModemResult::kSendRejected) {
          // UI gets only the stable stage code — the raw modem reply can
          // echo payload-bearing diagnostics.
          message = F("The modem rejected the message [stage=");
          message += stage;
          message += ']';
        } else if (result == ModemResult::kTimeout) {
          message = F("Send timed out [stage=");
          message += stage;
          message += ']';
        } else {
          message = F("Send failed [stage=");
          message += stage;
          message += ']';
        }
      }
    }
  }
  // #endregion BLOCK_attemptAtSend

  // #region BLOCK_sendCleanup
  if (lockHeld) {
    // Preserve the existing ModemResult → UI/result-token mapping.
    resultToken = result == ModemResult::kSuccess        ? "success"
                  : result == ModemResult::kSendRejected ? "send_rejected"
                  : result == ModemResult::kTimeout      ? "timeout"
                                                         : "protocol_error";
  }
  if (transportStarted) transport.end();
  if (lockHeld) modem_lock::give();
  free(scratch);
  // Publish final send state BEFORE restoring the poll task so the first new
  // poll cycle never observes a still-active send.
  sendMessage_ = message;
  sendSuccess_ = lockHeld && result == ModemResult::kSuccess;
  sendRunning_ = false;
  sendDone_ = true;
  Serial.printf("event=modem_send_complete result=%s stage=%s elapsed_ms=%lu heap=%u\n",
                resultToken, stage, millis() - startedAt, static_cast<unsigned>(ESP.getFreeHeap()));
  // Restore the poll lifecycle on every outcome of a clean stop; after a
  // stop timeout the old task may still run, so no duplicate is started.
  if (pollStopped) syncTask();
  // #endregion BLOCK_sendCleanup
}
// #endregion METHOD_ModemService_runSend
