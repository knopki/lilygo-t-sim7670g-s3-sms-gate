// #region MODULE_CONTRACT
// PURPOSE: Lets the SMS gateway join a WPA2/WPA3-Personal Wi-Fi network while
// providing a recoverable local web interface for network and SMTP delivery
// configuration.
// SCOPE:
// - Wi-Fi provisioning, captive portal, Digest-authenticated configuration,
// isolated persistent configuration, HTTP routes, and the asynchronous
// SMTP test-delivery lifecycle.
// - NOT: The SMTP dialog itself (smtp_client), TLS transport (smtp_transport),
// SMS, GNSS, and OTA logic.
// INVARIANTS: Credentials are never written to Serial or
// returned in HTTP responses; the SMTP password is never serialized.
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

#include "config_store.h"
#include "smtp_client.h"
#include "smtp_transport.h"
#include "web_api.h"

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
SmtpConfigStore smtpConfigStore;
RuntimeConfig config;
RuntimeSmtpConfig storedSmtpConfig;
bool smtpConfigLoaded = false;
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

// One asynchronous SMTP test delivery at a time; started by an HTTP route,
// executed by a dedicated task so the dialog never blocks loop() or the web
// server, and every failure is traceable to one stage plus reply code.
RuntimeSmtpConfig smtpTestCandidate;
String smtpTestEhloName;
volatile bool smtpTestRunning = false;
volatile bool smtpTestDone = false;
SmtpSendResult smtpTestResult = SmtpSendResult::kConnectFailed;
String smtpTestMessage;
String smtpTestFailedStage;
int smtpTestReplyCode = 0;

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

// #region FUNC_buildStatus
// PURPOSE: Snapshots controller state for the JSON API consumed by the
// browser UI.
WebStatus buildStatus() {
  WebStatus status;
  status.setupRequired = config.ssid.length() == 0;
  switch (connectionState) {
    case ConnectionState::kOnline:
      status.mode = F("sta");
      break;
    case ConnectionState::kConnecting:
      status.mode = F("connecting");
      break;
    case ConnectionState::kFallbackAp:
      status.mode = F("fallback_ap");
      break;
    default:
      status.mode = F("initial");
      break;
  }
  status.stationConnected = WiFi.status() == WL_CONNECTED;
  status.ssid = config.ssid;
  status.stationIp = status.stationConnected ? WiFi.localIP().toString() : String();
  status.macAddress = stationMacAddress;
  status.rssiDbm = status.stationConnected ? WiFi.RSSI() : 0;
  status.mdnsHostname = mdnsHostname;
  status.lastError = lastConnectionError;
  return status;
}
// #endregion FUNC_buildStatus

// #region FUNC_buildScanNetworksJson
// PURPOSE: Lists scanned WPA2/WPA3-Personal SSIDs as JSON while manual SSID
// entry in the browser UI keeps hidden networks reachable.
String buildScanNetworksJson() {
  Serial.println("event=wifi_scan_begin");
  String json;
  json.reserve(512);
  json += F("{\"networks\":[");
  const int networkCount = WiFi.scanNetworks(false, true);
  Serial.printf("event=wifi_scan_complete networks=%d\n", networkCount);
  bool firstEntry = true;
  for (int index = 0; index < networkCount; ++index) {
    const wifi_auth_mode_t security = WiFi.encryptionType(index);
    if (security != WIFI_AUTH_WPA2_PSK && security != WIFI_AUTH_WPA3_PSK &&
        security != WIFI_AUTH_WPA2_WPA3_PSK) {
      continue;
    }
    const String ssid = WiFi.SSID(index);
    if (ssid.length() == 0) {
      continue;
    }
    if (!firstEntry) {
      json += ',';
    }
    firstEntry = false;
    json += F("{\"ssid\":");
    appendJsonString(json, ssid);
    json += F(",\"rssi_dbm\":");
    json += String(WiFi.RSSI(index));
    json += F(",\"security\":\"");
    json += security == WIFI_AUTH_WPA3_PSK
                ? F("wpa3")
                : (security == WIFI_AUTH_WPA2_WPA3_PSK ? F("wpa2_wpa3") : F("wpa2"));
    json += F("\"}");
  }
  WiFi.scanDelete();
  json += F("]}");
  return json;
}
// #endregion FUNC_buildScanNetworksJson

// #region FUNC_handleStatusRequest
// PURPOSE: Reports controller state as JSON; open only while no verified
// configuration exists and Digest-protected afterwards.
void handleStatusRequest() {
  if (config.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  sendJson(server, 200, renderStatusJson(buildStatus()));
}
// #endregion FUNC_handleStatusRequest

// #region FUNC_handleScanRequest
// PURPOSE: Returns scanned WPA2/WPA3-Personal networks as the JSON payload the
// browser UI renders for selection.
void handleScanRequest() {
  if (config.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  sendJson(server, 200, buildScanNetworksJson());
}
// #endregion FUNC_handleScanRequest

// #region FUNC_sendJsonError
// PURPOSE: Emits the uniform error envelope the browser UI displays inline.
void sendJsonError(int code, const String& error) {
  sendJson(server, code, renderErrorJson(error));
}
// #endregion FUNC_sendJsonError

// #region FUNC_readCandidateConfig
// PURPOSE: Validates form data before it can interrupt the current station
// connection for the required temporary connection test.
bool readCandidateConfig(RuntimeConfig& candidate, String& error) {
  candidate.ssid = server.arg("ssid");
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
    sendJsonError(403, F("Initial setup is already complete."));
    return;
  }
  RuntimeConfig candidate;
  String error;
  if (!readCandidateConfig(candidate, error)) {
    sendJsonError(400, error);
    return;
  }
  candidate.adminPassword = server.arg("admin_password");
  const String confirmation = server.arg("admin_password_confirm");
  if (!isValidPassword(candidate.adminPassword) ||
      !constantTimeEquals(candidate.adminPassword, confirmation)) {
    sendJsonError(
        400, F("Administrator passwords must match and contain 8–63 printable ASCII characters."));
    return;
  }
  if (!testStationCandidate(candidate)) {
    lastConnectionError = F("Could not connect with those Wi-Fi credentials. Nothing was saved.");
    sendJsonError(400, lastConnectionError);
    return;
  }
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendJsonError(500, lastConnectionError);
    return;
  }

  config = candidate;
  onStationConnected(true);
  String message = F("Configuration saved. The access point will close shortly. Open http://");
  message += mdnsHostname;
  message += F(".local on the configured network.");
  sendJson(server, 200, renderMessageJson(message));
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
    sendJsonError(400, error);
    return;
  }
  if (!testStationCandidate(candidate)) {
    lastConnectionError =
        F("Could not connect with those Wi-Fi credentials. "
          "The previous profile was kept.");
    sendJsonError(400, lastConnectionError);
    return;
  }
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendJsonError(500, lastConnectionError);
    return;
  }

  config = candidate;
  onStationConnected(true);
  String message = F("Configuration saved. The interface is now available at http://");
  message += mdnsHostname;
  message += F(".local.");
  sendJson(server, 200, renderMessageJson(message));
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
    sendJsonError(400, F("The current administrator password is incorrect."));
    return;
  }
  if (!isValidPassword(newPassword) || !constantTimeEquals(newPassword, confirmation)) {
    sendJsonError(400, F("New passwords must match and contain 8–63 printable ASCII characters."));
    return;
  }

  RuntimeConfig candidate = config;
  candidate.adminPassword = newPassword;
  if (!configStore.save(candidate)) {
    lastConnectionError = F("Configuration could not be saved.");
    sendJsonError(500, lastConnectionError);
    return;
  }
  config = candidate;
  if (accessPointActive) {
    accessPointRestartAt = millis() + kApShutdownDelayMs;
  }
  sendJson(server, 200,
           renderMessageJson(F("Password changed. The browser will ask for the new password on "
                               "the next request.")));
}
// #endregion FUNC_handlePasswordSubmission

// #region FUNC_smtpSecurityName
// PURPOSE: Maps the security mode onto the stable JSON/HTML token.
const char* smtpSecurityName(SmtpSecurityMode mode) {
  return mode == SmtpSecurityMode::kImplicitTls ? "implicit" : "starttls";
}
// #endregion FUNC_smtpSecurityName

// #region FUNC_smtpResultName
// PURPOSE: Maps a send outcome onto the stable JSON token consumed by the UI.
const char* smtpResultName(SmtpSendResult result) {
  switch (result) {
    case SmtpSendResult::kSuccess:
      return "success";
    case SmtpSendResult::kConnectFailed:
      return "connect_failed";
    case SmtpSendResult::kTlsUnavailable:
      return "tls_unavailable";
    case SmtpSendResult::kTlsFailed:
      return "tls_failed";
    case SmtpSendResult::kAuthRejected:
      return "auth_rejected";
    case SmtpSendResult::kMessageRejected:
      return "message_rejected";
    default:
      return "dialog_failed";
  }
}
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

// #region FUNC_logSmtpStage
// PURPOSE: Traces one protocol step with reply code and free heap so a
// failing dialog is diagnosable from the Serial log alone.
void logSmtpStage(const char* stage, int code) {
  Serial.printf("event=smtp_stage name=%s code=%d heap=%u max_alloc=%u\n", stage, code,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
}
// #endregion FUNC_logSmtpStage

// #region FUNC_smtpTestTask
// PURPOSE: Runs one test delivery on its own task; copies the request before
// starting the network dialog and publishes the outcome last.
void smtpTestTask(void*) {
  const RuntimeSmtpConfig candidate = smtpTestCandidate;
  const String ehloName = smtpTestEhloName;
  const unsigned long startedAt = millis();
  Serial.printf("event=smtp_test_begin host=%s port=%u mode=%s heap=%u\n", candidate.host.c_str(),
                candidate.port, smtpSecurityName(candidate.securityMode),
                static_cast<unsigned>(ESP.getFreeHeap()));

  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  const SmtpConfigRecord record = buildSmtpConfigRecord(candidate);
  smtpTestResult = client.sendMail(record, ehloName.c_str(), "SMS Gate test message",
                                   "This is a test message from the SMS Gate device. "
                                   "If you can read it, SMTP delivery is working.");
  String message = smtpResultMessage(smtpTestResult);
  if (smtpTestResult != SmtpSendResult::kSuccess) {
    message += F(" [stage=");
    message += client.failedStage();
    if (client.lastReplyCode() != 0) {
      message += F(" code=");
      message += String(client.lastReplyCode());
    }
    message += ']';
  }
  smtpTestMessage = message;
  smtpTestFailedStage = client.failedStage();
  smtpTestReplyCode = client.lastReplyCode();
  Serial.printf(
      "event=smtp_test_complete result=%s stage=%s code=%d detail=%c errno=%d "
      "elapsed_ms=%lu heap=%u\n",
      smtpResultName(smtpTestResult), client.failedStage(), client.lastReplyCode(),
      channel.readDetail(), channel.lastErrno(), millis() - startedAt,
      static_cast<unsigned>(ESP.getFreeHeap()));
  smtpTestRunning = false;
  smtpTestDone = true;  // Published last: readers treat done as data-ready.
  vTaskDelete(nullptr);
}
// #endregion FUNC_smtpTestTask

// #region FUNC_parseSmtpPort
// PURPOSE: Accepts only a complete decimal port and falls back to the mode's
// standard port when the field is empty.
bool parseSmtpPort(const String& raw, SmtpSecurityMode mode, uint16_t& port) {
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
// #endregion FUNC_parseSmtpPort

// #region FUNC_readSmtpForm
// PURPOSE: Validates the SMTP form into a runtime profile; an empty password
// keeps the stored one, and every other field must pass the record rules.
bool readSmtpForm(RuntimeSmtpConfig& candidate, String& error) {
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
    if (!smtpConfigLoaded || storedSmtpConfig.password.length() == 0) {
      error = F("Enter the SMTP password.");
      return false;
    }
    candidate.password = storedSmtpConfig.password;
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
// #endregion FUNC_readSmtpForm

// #region FUNC_buildWebSmtpConfig
// PURPOSE: Snapshots the stored SMTP profile for the JSON API without ever
// serializing the password.
WebSmtpConfig buildWebSmtpConfig() {
  WebSmtpConfig web;
  web.present = smtpConfigLoaded && storedSmtpConfig.host.length() > 0;
  web.host = web.present ? storedSmtpConfig.host : String();
  web.port = web.present
                 ? storedSmtpConfig.port
                 : (storedSmtpConfig.securityMode == SmtpSecurityMode::kImplicitTls ? 465 : 587);
  web.security =
      web.present ? String(smtpSecurityName(storedSmtpConfig.securityMode)) : String(F("starttls"));
  web.username = web.present ? storedSmtpConfig.username : String();
  web.passwordSet = web.present && storedSmtpConfig.password.length() > 0;
  web.fromAddress = web.present ? storedSmtpConfig.fromAddress : String();
  web.recipientAddress = web.present ? storedSmtpConfig.recipientAddress : String();
  return web;
}
// #endregion FUNC_buildWebSmtpConfig

// #region FUNC_handleSmtpConfigRequest
// PURPOSE: Returns the stored SMTP profile (without the password) for form
// prefill in the browser UI.
void handleSmtpConfigRequest() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server, 200, renderSmtpConfigJson(buildWebSmtpConfig()));
}
// #endregion FUNC_handleSmtpConfigRequest

// #region FUNC_handleSmtpSaveSubmission
// PURPOSE: Validates and persists the SMTP profile; the password field may be
// empty to keep the stored one.
void handleSmtpSaveSubmission() {
  Serial.println("event=http_smtp_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeSmtpConfig candidate;
  String error;
  if (!readSmtpForm(candidate, error)) {
    sendJsonError(400, error);
    return;
  }
  if (!smtpConfigStore.save(candidate)) {
    sendJsonError(500, F("The SMTP configuration could not be saved."));
    return;
  }
  storedSmtpConfig = candidate;
  smtpConfigLoaded = true;
  smtpTestDone = false;
  smtpTestMessage = "";
  Serial.println("event=smtp_saved");
  sendJson(server, 200,
           renderMessageJson(F("SMTP settings saved. Use the test button to verify delivery.")));
}
// #endregion FUNC_handleSmtpSaveSubmission

// #region FUNC_handleSmtpTestStart
// PURPOSE: Starts one test delivery with the submitted form values on a
// dedicated task so loop() and the HTTP server stay responsive during TLS.
void handleSmtpTestStart() {
  Serial.println("event=http_smtp_test_submit");
  if (!requireAuthentication()) {
    return;
  }
  if (smtpTestRunning) {
    sendJsonError(409, F("A test delivery is already in progress."));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(400, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  RuntimeSmtpConfig candidate;
  String error;
  if (!readSmtpForm(candidate, error)) {
    sendJsonError(400, error);
    return;
  }
  smtpTestCandidate = candidate;
  smtpTestEhloName = mdnsHostname + F(".local");
  smtpTestDone = false;
  smtpTestMessage = "";
  smtpTestRunning = true;
  if (xTaskCreatePinnedToCore(smtpTestTask, "smtp_test", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    smtpTestRunning = false;
    Serial.println("event=smtp_test_failed reason=task_create");
    sendJsonError(503, F("The test could not be started. Try again."));
    return;
  }
  sendJson(server, 200, renderMessageJson(F("Test delivery started.")));
}
// #endregion FUNC_handleSmtpTestStart

// #region FUNC_handleSmtpTestStatus
// PURPOSE: Reports asynchronous test progress to the polling browser UI.
void handleSmtpTestStatus() {
  if (!requireAuthentication()) {
    return;
  }
  WebSmtpTest test;
  test.running = smtpTestRunning;
  test.done = smtpTestDone;
  if (smtpTestDone) {
    test.result = smtpResultName(smtpTestResult);
    test.message = smtpTestMessage;
  }
  sendJson(server, 200, renderSmtpTestJson(test));
}
// #endregion FUNC_handleSmtpTestStatus

// #region FUNC_handleNotFound
// PURPOSE: Redirects captive-portal DNS requests to the appropriate local page
// while returning a normal 404 when the device is only serving its STA address.
void handleNotFound() {
  if (accessPointActive) {
    server.sendHeader("Location", "/");
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
  server.on("/", HTTP_GET, []() { sendAsset(server, "/"); });
  server.on("/app.js", HTTP_GET, []() { sendAsset(server, "/app.js"); });
  server.on("/style.css", HTTP_GET, []() { sendAsset(server, "/style.css"); });
  server.on("/api/status", HTTP_GET, handleStatusRequest);
  server.on("/api/scan", HTTP_GET, handleScanRequest);
  server.on("/api/setup", HTTP_POST, handleSetupSubmission);
  server.on("/api/network", HTTP_POST, handleNetworkSubmission);
  server.on("/api/password", HTTP_POST, handlePasswordSubmission);
  server.on("/api/smtp", HTTP_GET, handleSmtpConfigRequest);
  server.on("/api/smtp", HTTP_POST, handleSmtpSaveSubmission);
  server.on("/api/smtp/test", HTTP_POST, handleSmtpTestStart);
  server.on("/api/smtp/test", HTTP_GET, handleSmtpTestStatus);
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
  smtpConfigLoaded = smtpConfigStore.load(storedSmtpConfig);
  recordBootStage(String(F("event=boot_smtp_config_loaded present=")) +
                  (smtpConfigLoaded ? F("true") : F("false")));

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
