// #region MODULE_CONTRACT
// PURPOSE: Keeps boot and loop orchestration separate from service behavior.
// SCOPE:
// - Boot trace, service wiring, setupFirmware, and loopFirmware.
// - NOT: Protocol dialogs, persistence details, HTTP routes, or UI rendering.
// INVARIANTS: Secrets stay out of logs and HTTP; modem operations are single-flight.
// #endregion MODULE_CONTRACT

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include "persistence/config_store.h"
#include "gps/gps_service.h"
#include "system/http_server.h"
#include "modem/modem_service.h"
#include "smtp/smtp_service.h"
#include "system/ntp_server.h"
#include "system/time_sync.h"
#include "system/watchdog.h"
#include "system/wifi_manager.h"
#include "zte/zte_service.h"

namespace {

constexpr unsigned long kSerialHeartbeatIntervalMs = 5UL * 1000UL;
constexpr size_t kBootTraceCapacity = 1024;
constexpr unsigned long kBootDelayMs = 1500;

WebServer server(kHttpPort);
ConfigStore configStore;
RuntimeConfig config;
SmtpService smtpService;
ZteService zteService;
WifiManager wifiManager;
ModemService modemService;
GpsService gpsService;
TimeSync timeSync;
NtpServer ntpServer(timeSync);
HttpServer httpServer(server, configStore, config, wifiManager, smtpService, zteService,
                      modemService, gpsService, timeSync);
unsigned long lastSerialHeartbeatAt = 0;
String bootTrace;
bool bootTraceCollecting = true;
bool bootTraceReplayed = false;

// #region FUNC_recordBootStage
// PURPOSE: Preserves startup events until native USB CDC becomes ready.
void recordBootStage(const String& event) {
  Serial.println(event);
  if (!bootTraceCollecting) {
    return;
  }
  if (bootTrace.length() + event.length() + 1 <= kBootTraceCapacity) {
    bootTrace += event;
    bootTrace += '\n';
  }
}
// #endregion FUNC_recordBootStage

// #region FUNC_setupFirmware
// PURPOSE: Starts the web server and either joins the verified Wi-Fi
// profile or exposes the first-time captive portal.
void setupFirmware() {
  Serial.begin(115200);
  delay(kBootDelayMs);
  watchdog::begin();
  recordBootStage(String(F("event=boot_started reset_reason=")) +
                  String(static_cast<int>(esp_reset_reason())));
  if (watchdog::isSafeMode()) {
    recordBootStage(String(F("event=watchdog_safe_mode boot_count=")) +
                    String(watchdog::bootLoopCount()) + F(" action=poll_tasks_paused"));
  }

  wifiManager.initIdentity();
  recordBootStage(String(F("event=boot_identity mac=")) + wifiManager.stationMacAddress() +
                  F(" ap=") + wifiManager.accessPointSsid() + F(" mdns=") +
                  wifiManager.mdnsHostname() + F(".local"));
  recordBootStage(F("event=boot_config_load_begin"));

  if (configStore.load(config)) {
    recordBootStage(F("event=boot_config_loaded action=station_connect"));
    wifiManager.beginStationAttempt(config);
  } else {
    recordBootStage(F("event=boot_config_missing action=initial_ap"));
    config = RuntimeConfig{};
    wifiManager.setConnectionState(WifiManager::ConnectionState::kInitialSetup);
    wifiManager.startAccessPoint(config);
    recordBootStage(String(F("event=boot_initial_ap_complete active=")) +
                    (wifiManager.accessPointActive() ? F("true") : F("false")));
  }
  const bool smtpLoaded = smtpService.load();
  recordBootStage(String(F("event=boot_smtp_config_loaded present=")) +
                  (smtpLoaded ? F("true") : F("false")));
  const bool zteLoaded = zteService.load();
  {
    String zteBoot = String(F("event=boot_zte_config_loaded present=")) +
                     (zteLoaded ? String(F("true")) : String(F("false")));
    if (zteLoaded) {
      zteBoot += F(" module=");
      zteBoot += zteService.config().moduleEnabled ? F("true") : F("false");
      zteBoot += F(" forward=");
      zteBoot += zteService.config().forwardEnabled ? F("true") : F("false");
      zteBoot += F(" poll_interval=");
      zteBoot += String(zteService.config().pollIntervalSec);
    }
    recordBootStage(zteBoot);
  }
  const bool modemLoaded = modemService.load();
  {
    String modemBoot = String(F("event=boot_modem_source_loaded present=")) +
                       (modemLoaded ? String(F("true")) : String(F("false")));
    if (modemLoaded) {
      modemBoot += F(" module=");
      modemBoot += modemService.config().moduleEnabled ? F("true") : F("false");
      modemBoot += F(" poll=");
      modemBoot += modemService.config().pollEnabled ? F("true") : F("false");
      modemBoot += F(" sms_poll=");
      modemBoot += modemService.config().smsPollEnabled ? F("true") : F("false");
      modemBoot += F(" poll_interval=");
      modemBoot += String(modemService.config().pollIntervalSec);
    }
    recordBootStage(modemBoot);
  }
  const bool gpsLoaded = gpsService.load();
  {
    String gpsBoot = String(F("event=boot_gps_config_loaded present=")) +
                     (gpsLoaded ? String(F("true")) : String(F("false")));
    if (gpsLoaded) {
      gpsBoot += F(" module=");
      gpsBoot += gpsService.config().moduleEnabled ? F("true") : F("false");
      gpsBoot += F(" poll=");
      gpsBoot += gpsService.config().pollEnabled ? F("true") : F("false");
      gpsBoot += F(" poll_interval=");
      gpsBoot += String(gpsService.config().pollIntervalSec);
    }
    recordBootStage(gpsBoot);
  }

  recordBootStage(F("event=boot_http_routes_begin"));
  timeSync.begin();
  timeSync.setGpsPollMs(gpsService.pollIntervalMs());
  timeSync.setModemPollMs(modemService.pollIntervalMs());
  wifiManager.setTimeSync(&timeSync);
  gpsService.setTimeSync(&timeSync);
  modemService.setTimeSync(&timeSync);
  httpServer.begin();
  zteService.setSmtpService(&smtpService);
  zteService.setWifiManager(&wifiManager);
  modemService.setSmtpService(&smtpService);
  modemService.setWifiManager(&wifiManager);
  modemService.setZteService(&zteService);
  if (!watchdog::isSafeMode()) {
    zteService.syncPollTask(zteService.shouldRunModule());
  } else {
    recordBootStage(F("event=poll_tasks_skipped reason=safe_mode"));
  }
  recordBootStage(F("event=modem_init_begin variant=classic"));
  if (!watchdog::isSafeMode()) {
    modemService.syncTask();
    gpsService.syncTask();
  }
  ntpServer.begin();
  recordBootStage(F("event=boot_http_routes_complete"));
  bootTraceCollecting = false;
}
// #endregion FUNC_setupFirmware

// #region FUNC_loopFirmware
// PURPOSE: Services HTTP/DNS and maintains the STA-to-fallback-AP lifecycle.
void loopFirmware() {
  watchdog::feedLoop();
  watchdog::loop();
  httpServer.handleClient();
  wifiManager.handleDns();

  const unsigned long now = millis();
  if (!bootTraceReplayed && now >= kSerialHeartbeatIntervalMs) {
    Serial.println("event=boot_trace_replay_begin");
    Serial.print(bootTrace);
    Serial.println("event=boot_trace_replay_complete");
    bootTraceReplayed = true;
  }
  if (now - lastSerialHeartbeatAt >= kSerialHeartbeatIntervalMs) {
    lastSerialHeartbeatAt = now;
    const char* mode =
        wifiManager.connectionState() == WifiManager::ConnectionState::kOnline
            ? "STA"
            : (wifiManager.connectionState() == WifiManager::ConnectionState::kConnecting
                   ? "connecting"
                   : (wifiManager.connectionState() == WifiManager::ConnectionState::kFallbackAp
                          ? "fallback-ap"
                          : "initial-ap"));
    const ModemStatus modemSnapshot = modemService.readStatus();
    const char* modemState =
        !modemSnapshot.present
            ? "offline"
            : (strcmp(modemSnapshot.cpin, "READY") != 0
                   ? "no-sim"
                   : (modemSnapshot.ceregStat != 1 && modemSnapshot.ceregStat != 5 &&
                              modemSnapshot.cregStat != 1 && modemSnapshot.cregStat != 5
                          ? "no-signal"
                          : "ready"));
    Serial.printf("firmware alive; mode=%s modem=%s\n", mode, modemState);
  }

  wifiManager.loop(config);
  timeSync.loop();
  ntpServer.loop();
}
// #endregion FUNC_loopFirmware

}  // namespace

void setup() { setupFirmware(); }

void loop() { loopFirmware(); }
