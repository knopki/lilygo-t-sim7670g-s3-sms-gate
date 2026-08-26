// #region MODULE_CONTRACT
// PURPOSE: Boots the SMS gateway, drives the Wi-Fi lifecycle and
// delegates HTTP, SMTP, ZTE and modem work to owned services so the
// sketch stays a thin setup/loop shell (target ~250 lines).
// SCOPE:
// - Boot trace, Serial heartbeat, service construction, setupFirmware
//   station/AP selection, NVS load for SMTP/ZTE/modem, cross-service
//   wiring, poll-task alignment, and loopFirmware request servicing.
// - NOT: HTTP route table, Digest authentication, JSON rendering, SMTP
//   TLS, ZTE goform, modem AT, asset gzip generation, and email
//   rendering — all owned by library modules.
// INVARIANTS: Credentials never written to Serial or returned in HTTP;
// one poll/test/send owns the modem at a time; boot events are
// preserved through the in-memory trace until USB CDC is ready.
// DEPENDENCIES: Uses Arduino-ESP32 WiFi/WebServer/esp_system;
// delegates to WifiManager, ConfigStore, SmtpService, ZteService,
// ModemService and HttpServer.
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
HttpServer httpServer(server, configStore, config, wifiManager, smtpService, zteService,
                      modemService, gpsService);
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
  recordBootStage(String(F("event=boot_started reset_reason=")) +
                  String(static_cast<int>(esp_reset_reason())));

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
      zteBoot += F(" enabled=");
      zteBoot += zteService.config().enabled ? F("true") : F("false");
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
      modemBoot += F(" enabled=");
      modemBoot += modemService.config().enabled ? F("true") : F("false");
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
      gpsBoot += F(" enabled=");
      gpsBoot += gpsService.config().enabled ? F("true") : F("false");
      gpsBoot += F(" poll_interval=");
      gpsBoot += String(gpsService.config().pollIntervalSec);
    }
    recordBootStage(gpsBoot);
  }

  recordBootStage(F("event=boot_http_routes_begin"));
  httpServer.begin();
  zteService.setSmtpService(&smtpService);
  zteService.setWifiManager(&wifiManager);
  modemService.setSmtpService(&smtpService);
  modemService.setWifiManager(&wifiManager);
  modemService.setZteService(&zteService);
  {
    const bool smtpReady = smtpService.isLoaded() && smtpService.config().host.length() > 0 &&
                           smtpService.config().password.length() > 0;
    zteService.syncPollTask(zteService.shouldRunPoll(smtpReady));
  }
  recordBootStage(F("event=modem_init_begin variant=classic"));
  modemService.syncTask();
  gpsService.syncTask();
  recordBootStage(F("event=boot_http_routes_complete"));
  bootTraceCollecting = false;
}
// #endregion FUNC_setupFirmware

// #region FUNC_loopFirmware
// PURPOSE: Services HTTP/DNS and maintains the STA-to-fallback-AP lifecycle.
void loopFirmware() {
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
}
// #endregion FUNC_loopFirmware

}  // namespace

void setup() { setupFirmware(); }

void loop() { loopFirmware(); }
