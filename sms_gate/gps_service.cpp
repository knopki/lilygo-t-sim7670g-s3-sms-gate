// #region MODULE_CONTRACT
// PURPOSE: Implements GpsService so sms_gate.ino only drives sync/load/save.
// #endregion MODULE_CONTRACT

#include "gps/gps_service.h"

#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

#include "modem/modem_transport.h"
#include "persistence/config_store_common.h"
#include "system/modem_lock.h"
#include "system/task_control.h"

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

// #region METHOD_GpsService_webConfig
WebGpsConfig GpsService::webConfig() const {
  WebGpsConfig web;
  web.present = loaded_;
  web.enabled = loaded_ ? stored_.enabled : false;
  web.pollIntervalSec = loaded_ ? stored_.pollIntervalSec : kDefaultGpsPollSec;
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
  out.enabled = server.arg("enabled") == F("1");
  if (!parsePollInterval(server.arg("poll_interval"), out.pollIntervalSec, kMinGpsPollSec,
                         kMaxGpsPollSec, error)) {
    return false;
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
    // Cheap check before taking mutex: if not loaded or disabled, skip AT.
    bool shouldPoll = loaded_ && stored_.enabled;
    if (shouldPoll) {
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
          // Time sync: when we have a fix with valid ISO time, set system clock if drift >2s
          if (status.fix && status.isoTime[0] != '\0') {
            int Y = 0, M = 0, D = 0, h = 0, mi = 0, s = 0;
            if (sscanf(status.isoTime, "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &mi, &s) == 6) {
              // civil to epoch (UTC) — Howard Hinnant days_from_civil
              auto daysFromCivil = [](int y, int m, int d) -> int64_t {
                y -= m <= 2;
                const int era = (y >= 0 ? y : y - 399) / 400;
                const int yoe = y - era * 400;
                const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                return era * 146097LL + doe - 719468LL;
              };
              int64_t days = daysFromCivil(Y, M, D);
              int64_t epoch = days * 86400LL + h * 3600LL + mi * 60LL + s;
              struct timeval tv{};
              tv.tv_sec = (time_t)epoch;
              tv.tv_usec = 0;
              struct timeval now{};
              gettimeofday(&now, nullptr);
              long diff = (long)(tv.tv_sec - now.tv_sec);
              if (diff < -2 || diff > 2) {
                settimeofday(&tv, nullptr);
                Serial.printf("event=gps_time_sync iso=%s epoch=%ld diff=%ld\n", status.isoTime,
                              (long)epoch, diff);
              }
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
      // Publish not-powered placeholder so UI shows disabled state if never polled
      // Keep last status but ensure present flag reflects modem liveness via one AT per interval
      // when disabled? When disabled we don't poll, so no Serial1 use.
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
