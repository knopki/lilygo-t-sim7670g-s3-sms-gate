// #region MODULE_CONTRACT
// PURPOSE: Implements GpsService so sms_gate.ino only drives sync/load/save.
// #endregion MODULE_CONTRACT

#include "gps/gps_service.h"

#include <Arduino.h>

#include "gps/gps_client.h"
#include "modem/modem_transport.h"
#include "persistence/config_store_common.h"
#include "system/modem_lock.h"
#include "system/task_control.h"
#include "system/time_sync.h"

namespace {
using task_control::kPollSliceMs;
using task_control::kServiceTaskStack;
constexpr size_t kGpsScratchSize = 2048;
}  // namespace

GpsService::GpsService() = default;

// #region METHOD_GpsService_load
bool GpsService::load() {
  loaded_ = store_.load(stored_);
  return loaded_;
}
// #endregion METHOD_GpsService_load

// #region METHOD_GpsService_save
bool GpsService::save(const RuntimeGpsConfig& candidate) {
  if (!store_.save(candidate)) return false;
  stored_ = candidate;
  loaded_ = true;
  return true;
}
// #endregion METHOD_GpsService_save

bool GpsService::shouldRunTask() const { return loaded_ && stored_.moduleEnabled; }

bool GpsService::shouldPoll() const { return shouldRunTask() && stored_.pollEnabled; }

bool GpsService::shouldTimeSync() const { return shouldPoll() && stored_.timeSyncEnabled; }

// #region METHOD_GpsService_webConfig
WebGpsConfig GpsService::webConfig() const {
  WebGpsConfig web;
  web.present = loaded_;
  web.moduleEnabled = loaded_ ? stored_.moduleEnabled : false;
  web.pollEnabled = loaded_ ? stored_.pollEnabled : false;
  // compat: enabled = module && poll
  web.enabled = loaded_ ? (stored_.moduleEnabled && stored_.pollEnabled) : false;
  web.pollIntervalSec = loaded_ ? stored_.pollIntervalSec : kDefaultGpsPollSec;
  web.timeSyncEnabled = loaded_ ? stored_.timeSyncEnabled : true;
  // lastStatus derived from current GpsStatus fix/power for quick UI hint
  GpsStatus raw = statusCache_.read();
  if (raw.present) {
    if (raw.fix)
      web.lastStatus = String(F("fix sats=")) + String(raw.satsUsed) + F(" lat=") +
                       String(raw.lat, 6) + F(" lon=") + String(raw.lon, 6);
    else if (raw.powered)
      web.lastStatus = F("powered, no fix");
    else
      web.lastStatus = F("not powered");
  }
  return web;
}
// #endregion METHOD_GpsService_webConfig

// #region METHOD_GpsService_webStatus
WebGpsStatus GpsService::webStatus() const {
  GpsStatus raw = statusCache_.read();
  WebGpsStatus web;
  web.present = raw.present;
  web.powered = raw.powered;
  web.fix = raw.fix;
  web.mode = raw.mode;
  web.satsUsed = raw.satsUsed;
  web.satsVisible = raw.satsVisible;
  web.lat = raw.lat;
  web.lon = raw.lon;
  web.alt = raw.alt;
  web.speed = raw.speed;
  web.course = raw.course;
  web.date = String(raw.date);
  web.utcTime = String(raw.utcTime);
  web.isoTime = String(raw.isoTime);
  web.updatedMs = raw.updatedMs;
  return web;
}
// #endregion METHOD_GpsService_webStatus

// #region METHOD_GpsService_readForm
bool GpsService::readForm(WebServer& server, RuntimeGpsConfig& out, String& error) {
  out.moduleEnabled = server.arg("module_enabled") == F("1");
  // backward compat: if module_enabled missing but enabled present, map it
  if (!server.hasArg("module_enabled") && server.hasArg("enabled")) {
    out.moduleEnabled = server.arg("enabled") == F("1");
  }
  out.pollEnabled = server.arg("poll_enabled") == F("1");
  if (!server.hasArg("poll_enabled")) {
    // if only module flag given, default poll = module (migration friendliness)
    // but for web form both are present; this fallback covers old clients
    out.pollEnabled = out.moduleEnabled;
  }
  if (!parsePollInterval(server.arg("poll_interval"), out.pollIntervalSec, kMinGpsPollSec,
                         kMaxGpsPollSec, error)) {
    return false;
  }
  out.timeSyncEnabled = server.arg("time_sync") == F("1");
  if (!server.hasArg("time_sync")) {
    // old form used timeSyncEnabled implicitly true; keep true if missing
    out.timeSyncEnabled = true;
  }
  // enforce dependency: timeSync requires poll && module
  if (out.timeSyncEnabled && (!out.pollEnabled || !out.moduleEnabled)) {
    // allow but will be ineffective; don't reject, just log
  }
  return true;
}
// #endregion METHOD_GpsService_readForm

// #region METHOD_GpsService_pollIntervalMs
unsigned long GpsService::pollIntervalMs() const {
  const uint16_t sec = loaded_ ? stored_.pollIntervalSec : kDefaultGpsPollSec;
  if (!isValidGpsPollInterval(sec)) return kDefaultGpsPollSec * 1000UL;
  return static_cast<unsigned long>(sec) * 1000UL;
}
// #endregion METHOD_GpsService_pollIntervalMs

// #region METHOD_GpsService_publishStatus
void GpsService::publishStatus(const GpsStatus& status) { statusCache_.publish(status); }
// #endregion METHOD_GpsService_publishStatus

// #region METHOD_GpsService_readStatus
GpsStatus GpsService::readStatus() const { return statusCache_.read(); }
// #endregion METHOD_GpsService_readStatus

// #region METHOD_GpsService_runPollTask
void GpsService::runPollTask() {
  char* scratch = static_cast<char*>(malloc(kGpsScratchSize));
  if (scratch == nullptr) {
    Serial.println("event=gps_error stage=no_scratch");
    taskHandle_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("event=gps_task_started poll_interval=%lu heap=%u stack_hwm=%u\n", pollIntervalMs(),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  // Let the modem finish its power-on and initial AT init before first GNSS poll.
  for (unsigned long waited = 0; waited < 15000 && !taskStopRequested_; waited += kPollSliceMs) {
    vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
  }

  while (!taskStopRequested_) {
    if (shouldPoll()) {
      pollActive_ = true;
      bool gotLock = modem_lock::take(12000);
      if (!gotLock) {
        Serial.println("event=gps_poll_skipped reason=modem_busy");
      } else {
        ModemTransport transport;
        transport.begin();
        GpsClient client(transport, scratch, kGpsScratchSize);
        GpsStatus status;
        GpsResult result = client.poll(status);
        status.updatedMs = millis();
        if (result == GpsResult::kSuccess) {
          publishStatus(status);
          Serial.printf(
              "event=gps_poll present=%s powered=%s fix=%s mode=%d sats_used=%d sats_vis=%d "
              "lat=%.6f lon=%.6f alt=%.1f iso=%s stack_hwm=%u\n",
              status.present ? "true" : "false", status.powered ? "true" : "false",
              status.fix ? "true" : "false", status.mode, status.satsUsed, status.satsVisible,
              status.lat, status.lon, status.alt, status.isoTime,
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
          // Time sync: feed GNSS sample to TimeSync (ms precision) when fix & timeSyncEnabled.
          if (status.fix && shouldTimeSync() && timeSync_ != nullptr && status.isoTime[0] != '\0') {
            GpsFixFields tmp{};
            snprintf(tmp.date, sizeof(tmp.date), "%s", status.date);
            snprintf(tmp.utcTime, sizeof(tmp.utcTime), "%s", status.utcTime);
            tmp.timeMs = status.timeMs;
            int64_t epochMs = 0;
            if (gpsFixToEpochMs(tmp, epochMs)) {
              timeSync_->feedGnssSample(epochMs, 120);
              Serial.printf("event=gps_time_feed epoch_ms=%lld iso=%s ms=%d\n", (long long)epochMs,
                            status.isoTime, status.timeMs);
            }
          }
        } else {
          GpsStatus absent;
          absent.present = false;
          String safeStage = String(client.failedStage());
          safeStage.replace("=", "_");
          publishStatus(absent);
          Serial.printf("event=gps_error stage=%s\n", safeStage.c_str());
        }
        transport.end();
        modem_lock::give();
      }
      pollActive_ = false;
    } else {
      // Module disabled or poll disabled: don't use Serial1.
    }

    const unsigned long intervalMs = pollIntervalMs();
    for (unsigned long waited = 0; waited < intervalMs && !taskStopRequested_;
         waited += kPollSliceMs) {
      vTaskDelay(pdMS_TO_TICKS(kPollSliceMs));
    }
  }
  free(scratch);
  Serial.println("event=gps_task_stopped");
  taskHandle_ = nullptr;
  vTaskDelete(nullptr);
}
// #endregion METHOD_GpsService_runPollTask

// #region METHOD_GpsService_pollTask
void GpsService::pollTask(void* param) {
  auto* self = static_cast<GpsService*>(param);
  if (self != nullptr) self->runPollTask();
}
// #endregion METHOD_GpsService_pollTask

// #region METHOD_GpsService_syncTask
void GpsService::syncTask() {
  if (taskHandle_ != nullptr) {
    if (!task_control::stopTask(taskHandle_, taskStopRequested_)) {
      Serial.println("event=gps_task_stop_timeout");
      return;
    }
  }
  if (!shouldRunTask()) {
    Serial.println("event=gps_task_stopped reason=module_disabled");
    return;
  }
  if (xTaskCreatePinnedToCore(pollTask, "gps_poll", kServiceTaskStack, this, 1, &taskHandle_, 0) !=
      pdPASS) {
    taskHandle_ = nullptr;
    Serial.println("event=gps_task_start_failed reason=task_create");
    return;
  }
  Serial.println("event=gps_task_started");
}
// #endregion METHOD_GpsService_syncTask

// #region METHOD_GpsService_stopTask
bool GpsService::stopTask() {
  if (taskHandle_ == nullptr) return true;
  if (!task_control::stopTask(taskHandle_, taskStopRequested_)) {
    Serial.println("event=gps_task_stop_timeout");
    return false;
  }
  return true;
}
// #endregion METHOD_GpsService_stopTask
