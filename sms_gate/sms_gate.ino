// #region MODULE_CONTRACT
// PURPOSE: Lets the SMS gateway join a WPA2/WPA3-Personal Wi-Fi network while
// providing a recoverable local web interface for network configuration.
// SCOPE:
// - Wi-Fi provisioning, captive portal, Digest-authenticated
// configuration, and isolated persistent configuration.
// - NOT: SMS, GNSS, email, and OTA logic.
// INVARIANTS: Credentials are never written to Serial or
// returned in HTTP responses.
// DEPENDENCIES: Uses Arduino-ESP32 WiFi, WebServer, DNSServer, ESPmDNS,
// and Preferences.
// #endregion MODULE_CONTRACT

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>

#include "web_ui.h"

namespace {

constexpr char kAdminUser[] = "admin";
constexpr char kAuthRealm[] = "SMS Gate";
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kDnsPort = 53;
constexpr unsigned long kConnectTimeoutMs = 30UL * 1000UL;
constexpr unsigned long kReconnectIntervalMs = 60UL * 1000UL;
constexpr unsigned long kApShutdownDelayMs = 3UL * 1000UL;
constexpr unsigned long kSerialHeartbeatIntervalMs = 5UL * 1000UL;
constexpr size_t kBootTraceCapacity = 1024;

enum class ConnectionState { kInitialSetup, kConnecting, kOnline, kFallbackAp };

WebServer server(kHttpPort);
DNSServer dnsServer;
ConfigStore configStore;
RuntimeConfig config;
ConnectionState connectionState = ConnectionState::kInitialSetup;
String accessPointSsid;
String mdnsHostname;
String stationMacAddress;
String lastConnectionError;
bool accessPointActive = false;
bool mdnsActive = false;
unsigned long connectionAttemptStartedAt = 0;
unsigned long nextReconnectAt = 0;
unsigned long accessPointShutdownAt = 0;
unsigned long accessPointRestartAt = 0;
unsigned long lastSerialHeartbeatAt = 0;
String bootTrace;
bool bootTraceCollecting = true;
bool bootTraceReplayed = false;

// #region FUNC_recordBootStage
// PURPOSE: Preserves startup events until native USB CDC becomes ready, so the
// operator receives the complete boot path instead of only later heartbeats.
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

// #region FUNC_buildStationMacAddress
// PURPOSE: Reads the stable eFuse station MAC for the access-point whitelist
// instead of relying on an uninitialised Wi-Fi interface.
String buildStationMacAddress() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    return F("UNKNOWN");
  }
  char address[18] = {};
  snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return String(address);
}
// #endregion FUNC_buildStationMacAddress

// #region FUNC_buildDeviceSuffix
// PURPOSE: Produces a stable per-device suffix for the AP SSID and mDNS name.
String buildDeviceSuffix() {
  String compactMac = stationMacAddress;
  compactMac.replace(":", "");
  return compactMac.length() == 12 ? compactMac.substring(6) : String(F("UNKNOWN"));
}
// #endregion FUNC_buildDeviceSuffix

// #region FUNC_startMdns
// PURPOSE: Publishes a stable local HTTP hostname whenever the station has an
// IP address from the configured Wi-Fi network.
void startMdns() {
  if (mdnsActive) {
    MDNS.end();
    mdnsActive = false;
  }
  if (MDNS.begin(mdnsHostname.c_str())) {
    MDNS.addService("http", "tcp", kHttpPort);
    mdnsActive = true;
    Serial.printf("event=mdns_started hostname=%s.local\n", mdnsHostname.c_str());
  } else {
    lastConnectionError = F("Wi-Fi connected, but mDNS could not start.");
    Serial.println("event=mdns_failed");
  }
}
// #endregion FUNC_startMdns

// #region FUNC_startAccessPoint
// PURPOSE: Makes the configuration interface reachable when no profile exists
// or the saved station profile cannot connect.
void startAccessPoint() {
  const bool initialSetup = config.ssid.length() == 0;
  const wifi_mode_t mode = initialSetup ? WIFI_AP : WIFI_AP_STA;
  Serial.printf("event=ap_start mode=%s ssid=%s\n", initialSetup ? "AP" : "AP+STA",
                accessPointSsid.c_str());
  if (!WiFi.mode(mode)) {
    lastConnectionError = F("Could not select Wi-Fi AP mode.");
    Serial.println("event=ap_failed reason=mode_selection");
    return;
  }

  const bool started = initialSetup
                           ? WiFi.softAP(accessPointSsid.c_str())
                           : WiFi.softAP(accessPointSsid.c_str(), config.adminPassword.c_str());
  if (!started) {
    lastConnectionError = F("The configuration access point could not start.");
    Serial.println("event=ap_failed reason=softap");
    return;
  }

  dnsServer.stop();
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  accessPointActive = true;
  Serial.printf("event=ap_active ssid=%s ip=%s\n", accessPointSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}
// #endregion FUNC_startAccessPoint

// #region FUNC_stopAccessPoint
// PURPOSE: Removes the fallback wireless network after station connectivity is
// restored so it does not remain an unnecessary access path.
void stopAccessPoint() {
  if (!accessPointActive) {
    return;
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  accessPointActive = false;
  accessPointShutdownAt = 0;
  Serial.println("event=ap_stopped");
}
// #endregion FUNC_stopAccessPoint

// #region FUNC_beginStationAttempt
// PURPOSE: Starts the bounded connection attempt for the one verified saved
// Wi-Fi profile while retaining a fallback AP when it is already active.
void beginStationAttempt() {
  if (config.ssid.length() == 0) {
    return;
  }
  WiFi.mode(accessPointActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
  connectionState = ConnectionState::kConnecting;
  connectionAttemptStartedAt = millis();
  Serial.printf("event=sta_connect_begin fallback_ap=%s\n", accessPointActive ? "true" : "false");
}
// #endregion FUNC_beginStationAttempt

// #region FUNC_onStationConnected
// PURPOSE: Records healthy station connectivity and starts local name
// discovery.
void onStationConnected(bool deferAccessPointShutdown) {
  connectionState = ConnectionState::kOnline;
  nextReconnectAt = 0;
  lastConnectionError = "";
  startMdns();
  Serial.printf("event=sta_connected ip=%s\n", WiFi.localIP().toString().c_str());
  if (accessPointActive) {
    if (deferAccessPointShutdown) {
      accessPointShutdownAt = millis() + kApShutdownDelayMs;
    } else {
      stopAccessPoint();
    }
  }
}
// #endregion FUNC_onStationConnected

// #region FUNC_onStationFailed
// PURPOSE: Exposes a protected fallback AP after the agreed connection timeout
// and schedules later non-destructive reconnection attempts.
void onStationFailed() {
  lastConnectionError = F("Could not connect to the saved Wi-Fi network.");
  if (!accessPointActive) {
    startAccessPoint();
  }
  connectionState = ConnectionState::kFallbackAp;
  nextReconnectAt = millis() + kReconnectIntervalMs;
  Serial.println("event=sta_connect_failed action=fallback_ap");
}
// #endregion FUNC_onStationFailed

// #region FUNC_requireAuthentication
// PURPOSE: Guards all normal configuration and status routes with built-in HTTP
// Digest authentication.
bool requireAuthentication() {
  if (server.authenticate(kAdminUser, config.adminPassword.c_str())) {
    return true;
  }
  Serial.println("event=http_auth_rejected");
  server.requestAuthentication(DIGEST_AUTH, kAuthRealm);
  return false;
}
// #endregion FUNC_requireAuthentication

// #region FUNC_buildWifiNetworkOptions
// PURPOSE: Lists scanned WPA2/WPA3-Personal SSIDs while preserving manual SSID
// entry for hidden networks.
String buildWifiNetworkOptions() {
  Serial.println("event=wifi_scan_begin");
  String options;
  const int networkCount = WiFi.scanNetworks(false, true);
  Serial.printf("event=wifi_scan_complete networks=%d\n", networkCount);
  for (int index = 0; index < networkCount; ++index) {
    const wifi_auth_mode_t security = WiFi.encryptionType(index);
    if (security != WIFI_AUTH_WPA2_PSK && security != WIFI_AUTH_WPA3_PSK &&
        security != WIFI_AUTH_WPA2_WPA3_PSK) {
      continue;
    }
    const String ssid = WiFi.SSID(index);
    options += F("<option value='");
    options += escapeHtml(ssid);
    options += F("'>");
    options += escapeHtml(ssid);
    options += F(" (");
    options += String(WiFi.RSSI(index));
    options += F(" dBm)</option>");
  }
  WiFi.scanDelete();
  return options;
}
// #endregion FUNC_buildWifiNetworkOptions

// #region FUNC_sendSetupPage
// PURPOSE: Sends the isolated UI module's initial configuration document.
void sendSetupPage(const String& error = "", bool includeScanResults = false) {
  const String networkOptions = includeScanResults ? buildWifiNetworkOptions() : "";
  server.send(200, "text/html; charset=utf-8",
              renderSetupPage(networkOptions, error, stationMacAddress));
}
// #endregion FUNC_sendSetupPage

// #region FUNC_sendConfigPage
// PURPOSE: Supplies controller state to the isolated UI renderer.
void sendConfigPage(const String& message = "", const String& error = "",
                    bool includeScanResults = false) {
  WebStatus status;
  status.mode =
      connectionState == ConnectionState::kOnline
          ? F("STA")
          : (connectionState == ConnectionState::kConnecting ? F("Connecting") : F("Fallback AP"));
  status.stationIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : F("Not connected");
  status.macAddress = stationMacAddress;
  status.rssi = WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "Not available";
  status.mdnsHostname = mdnsHostname;
  status.lastError = lastConnectionError;
  const String networkOptions = includeScanResults ? buildWifiNetworkOptions() : "";
  server.send(200, "text/html; charset=utf-8",
              renderConfigPage(config, status, networkOptions, message, error));
}
// #endregion FUNC_sendConfigPage

// #region FUNC_readCandidateConfig
// PURPOSE: Validates form data before it can interrupt the current station
// connection for the required temporary connection test.
bool readCandidateConfig(RuntimeConfig& candidate, String& error) {
  candidate.ssid = server.arg("scanned_ssid");
  if (candidate.ssid.length() == 0) {
    candidate.ssid = server.arg("ssid");
  }
  candidate.wifiPassword = server.arg("wifi_password");
  if (candidate.ssid.length() == 0 || candidate.ssid.length() > kMaxSsidLength) {
    error = F("SSID must contain 1–32 characters.");
    return false;
  }
  if (!isValidPassword(candidate.wifiPassword)) {
    error = F("Wi-Fi password must contain 8–63 printable ASCII characters.");
    return false;
  }
  return true;
}
// #endregion FUNC_readCandidateConfig

// #region FUNC_testStationCandidate
// PURPOSE: Connects a candidate profile for at most 30 seconds without writing
// it, then restores the previous verified profile after a failed test.
bool testStationCandidate(const RuntimeConfig& candidate) {
  const RuntimeConfig previous = config;
  Serial.println("event=sta_candidate_test_begin");
  WiFi.mode(accessPointActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(candidate.ssid.c_str(), candidate.wifiPassword.c_str());
  const unsigned long deadline = millis() + kConnectTimeoutMs;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("event=sta_candidate_test_complete connected=true");
      return true;
    }
    delay(100);
  }

  Serial.println("event=sta_candidate_test_complete connected=false");
  if (previous.ssid.length() > 0) {
    Serial.println("event=sta_previous_profile_restore_begin");
    WiFi.disconnect();
    WiFi.begin(previous.ssid.c_str(), previous.wifiPassword.c_str());
    connectionState = ConnectionState::kConnecting;
    connectionAttemptStartedAt = millis();
  }
  return false;
}
// #endregion FUNC_testStationCandidate

// #region FUNC_handleSetupSubmission
// PURPOSE: Accepts initial configuration only after the requested Wi-Fi profile
// successfully connects and the administrator password is confirmed.
void handleSetupSubmission() {
  Serial.println("event=http_setup_submit");
  if (config.ssid.length() > 0) {
    server.send(403, "text/plain", "Initial setup is already complete.");
    return;
  }
  RuntimeConfig candidate;
  String error;
  if (!readCandidateConfig(candidate, error)) {
    sendSetupPage(error);
    return;
  }
  candidate.adminPassword = server.arg("admin_password");
  const String confirmation = server.arg("admin_password_confirm");
  if (!isValidPassword(candidate.adminPassword) ||
      !constantTimeEquals(candidate.adminPassword, confirmation)) {
    sendSetupPage(
        F("Administrator passwords must match and contain 8–63 "
          "printable ASCII characters."));
    return;
  }
  if (!testStationCandidate(candidate)) {
    lastConnectionError = F("Could not connect with those Wi-Fi credentials. Nothing was saved.");
    sendSetupPage(lastConnectionError);
    return;
  }
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendSetupPage(lastConnectionError);
    return;
  }

  config = candidate;
  onStationConnected(true);
  String message = F("Configuration saved. The access point will close shortly. Open http://");
  message += mdnsHostname;
  message += F(".local on the configured network.");
  sendConfigPage(message);
}
// #endregion FUNC_handleSetupSubmission

// #region FUNC_handleNetworkSubmission
// PURPOSE: Replaces the sole saved profile only after its credentials connect;
// on failure the existing profile remains the persisted configuration.
void handleNetworkSubmission() {
  Serial.println("event=http_network_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeConfig candidate = config;
  String error;
  if (!readCandidateConfig(candidate, error)) {
    sendConfigPage("", error);
    return;
  }
  if (!testStationCandidate(candidate)) {
    lastConnectionError =
        F("Could not connect with those Wi-Fi credentials. "
          "The previous profile was kept.");
    sendConfigPage("", lastConnectionError);
    return;
  }
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendConfigPage("", lastConnectionError);
    return;
  }

  config = candidate;
  onStationConnected(true);
  String message = F("Configuration saved. The interface is now available at http://");
  message += mdnsHostname;
  message += F(".local.");
  sendConfigPage(message);
}
// #endregion FUNC_handleNetworkSubmission

// #region FUNC_handlePasswordSubmission
// PURPOSE: Changes the shared administrator and fallback-AP password only when
// the currently authenticated owner also re-enters the existing password.
void handlePasswordSubmission() {
  Serial.println("event=http_password_submit");
  if (!requireAuthentication()) {
    return;
  }
  const String currentPassword = server.arg("current_password");
  const String newPassword = server.arg("new_password");
  const String confirmation = server.arg("new_password_confirm");
  if (!constantTimeEquals(currentPassword, config.adminPassword)) {
    sendConfigPage("", F("The current administrator password is incorrect."));
    return;
  }
  if (!isValidPassword(newPassword) || !constantTimeEquals(newPassword, confirmation)) {
    sendConfigPage("", F("New passwords must match and contain 8–63 printable "
                         "ASCII characters."));
    return;
  }

  RuntimeConfig candidate = config;
  candidate.adminPassword = newPassword;
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendConfigPage("", lastConnectionError);
    return;
  }
  config = candidate;
  if (accessPointActive) {
    accessPointRestartAt = millis() + kApShutdownDelayMs;
  }
  server.requestAuthentication(DIGEST_AUTH, kAuthRealm,
                               String(F("Password changed. Authenticate with the new password.")));
}
// #endregion FUNC_handlePasswordSubmission

// #region FUNC_handleNotFound
// PURPOSE: Redirects captive-portal DNS requests to the appropriate local page
// while returning a normal 404 when the device is only serving its STA address.
void handleNotFound() {
  if (accessPointActive) {
    server.sendHeader("Location", config.ssid.length() == 0 ? "/setup" : "/");
    server.send(302, "text/plain", "Redirecting to configuration.");
    return;
  }
  server.send(404, "text/plain", "Not found.");
}
// #endregion FUNC_handleNotFound

// #region FUNC_configureWebServer
// PURPOSE: Registers open initial-setup and protected normal-configuration
// routes before accepting HTTP requests from either network mode.
void configureWebServer() {
  Serial.println("event=http_routes_register_begin");
  server.on("/", HTTP_GET, []() {
    if (config.ssid.length() == 0) {
      sendSetupPage("", server.hasArg("scan"));
      return;
    }
    if (requireAuthentication()) {
      sendConfigPage("", "", server.hasArg("scan"));
    }
  });
  server.on("/setup", HTTP_GET, []() {
    if (config.ssid.length() == 0) {
      sendSetupPage("", server.hasArg("scan"));
    } else {
      server.send(403, "text/plain", "Initial setup is complete.");
    }
  });
  server.on("/setup", HTTP_POST, handleSetupSubmission);
  server.on("/network", HTTP_POST, handleNetworkSubmission);
  server.on("/password", HTTP_POST, handlePasswordSubmission);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("event=http_server_started port=%u\n", kHttpPort);
}
// #endregion FUNC_configureWebServer

// #region FUNC_setupFirmware
// PURPOSE: Starts the web server and either joins the verified Wi-Fi profile or
// exposes the first-time captive portal without requiring a reboot.
void setupFirmware() {
  Serial.begin(115200);
  delay(1500);
  recordBootStage(String(F("event=boot_started reset_reason=")) +
                  String(static_cast<int>(esp_reset_reason())));

  stationMacAddress = buildStationMacAddress();
  const String suffix = buildDeviceSuffix();
  accessPointSsid = String(F("SMS-Gate-")) + suffix;
  mdnsHostname = String(F("sms-gate-")) + suffix;
  recordBootStage(String(F("event=boot_identity mac=")) + stationMacAddress + F(" ap=") +
                  accessPointSsid + F(" mdns=") + mdnsHostname + F(".local"));
  recordBootStage(F("event=boot_config_load_begin"));

  if (configStore.load(config)) {
    recordBootStage(F("event=boot_config_loaded action=station_connect"));
    beginStationAttempt();
  } else {
    recordBootStage(F("event=boot_config_missing action=initial_ap"));
    config = RuntimeConfig{};
    connectionState = ConnectionState::kInitialSetup;
    startAccessPoint();
    recordBootStage(String(F("event=boot_initial_ap_complete active=")) +
                    (accessPointActive ? F("true") : F("false")));
  }

  recordBootStage(F("event=boot_http_routes_begin"));
  configureWebServer();
  recordBootStage(F("event=boot_http_routes_complete"));
  bootTraceCollecting = false;
}
// #endregion FUNC_setupFirmware

// #region FUNC_loopFirmware
// PURPOSE: Services HTTP/DNS requests and maintains the agreed STA-to-fallback-
// AP lifecycle with bounded connection attempts and periodic retries.
void loopFirmware() {
  server.handleClient();
  if (accessPointActive) {
    dnsServer.processNextRequest();
  }

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
        connectionState == ConnectionState::kOnline
            ? "STA"
            : (connectionState == ConnectionState::kConnecting
                   ? "connecting"
                   : (connectionState == ConnectionState::kFallbackAp ? "fallback-ap"
                                                                      : "initial-ap"));
    Serial.printf("firmware alive; mode=%s\n", mode);
  }

  if (connectionState == ConnectionState::kConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      onStationConnected(false);
    } else if (now - connectionAttemptStartedAt >= kConnectTimeoutMs) {
      onStationFailed();
    }
  } else if (connectionState == ConnectionState::kFallbackAp && config.ssid.length() > 0 &&
             now >= nextReconnectAt) {
    beginStationAttempt();
  }

  if (accessPointShutdownAt > 0 && now >= accessPointShutdownAt && WiFi.status() == WL_CONNECTED) {
    stopAccessPoint();
  }
  if (accessPointRestartAt > 0 && now >= accessPointRestartAt && accessPointActive) {
    accessPointRestartAt = 0;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    accessPointActive = false;
    startAccessPoint();
  }
}
// #endregion FUNC_loopFirmware

}  // namespace

void setup() { setupFirmware(); }

void loop() { loopFirmware(); }
