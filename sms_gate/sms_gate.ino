// #region MODULE_CONTRACT
// PURPOSE: Lets the SMS gateway join a WPA2/WPA3-Personal Wi-Fi network while
// providing a recoverable local web interface for network, SMTP delivery,
// and ZTE SMS source configuration, forwards ZTE modem SMS by email, and
// sends outgoing SMS through the same modem (ADR-0003).
// SCOPE:
// - Wi-Fi provisioning, captive portal, Digest-authenticated configuration,
// isolated persistent configuration, HTTP routes, the asynchronous SMTP
// test-delivery lifecycle, the ZTE poll/forward/delete lifecycle, and the
// asynchronous ZTE send lifecycle (recipient and body fields shared by
// future SMS sources).
// - NOT: The SMTP dialog itself (smtp_client), TLS transport
// (smtp_transport), the ZTE goform dialog (zte_client), its transport
// (zte_transport), SMS from the onboard modem, GNSS, and OTA logic.
// INVARIANTS: Credentials are never written to Serial or
// returned in HTTP responses; the SMTP and modem passwords are never
// serialized; a ZTE SMS is deleted on the modem only after SMTP acceptance.
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
#include <new>

#include "config_store.h"
#include "smtp_client.h"
#include "smtp_transport.h"
#include "web_api.h"
#include "zte_client.h"
#include "zte_transport.h"

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
constexpr unsigned long kZtePollIntervalMs = 15UL * 1000UL;
constexpr size_t kZteScratchSize = 20UL * 1024UL;
constexpr size_t kZteStatusLength = 160;
// The modem's send command is asynchronous: one status sample per second,
// bounded, mirrors its own web UI polling.
constexpr int kZteSendStatusAttempts = 20;
constexpr unsigned long kZteSendStatusDelayMs = 1000;
// NTP sources for the wall clock SEND_SMS validates (sms_time); the modem
// runs its own SNTP and rejects far-off timestamps.
constexpr const char* kNtpServers[] = {"pool.ntp.org", "time.nist.gov"};

enum class ConnectionState { kInitialSetup, kConnecting, kOnline, kFallbackAp };

WebServer server(kHttpPort);
DNSServer dnsServer;
ConfigStore configStore;
SmtpConfigStore smtpConfigStore;
ZteConfigStore zteConfigStore;
RuntimeConfig config;
RuntimeSmtpConfig storedSmtpConfig;
bool smtpConfigLoaded = false;
RuntimeZteConfig storedZteConfig;
bool zteConfigLoaded = false;
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

// One ZTE poll lifecycle: a long-lived task that forwards one oldest SMS per
// cycle and deletes it only after SMTP acceptance; the modem inbox is the
// only delivery state (ADR-0003). One asynchronous ZTE connection test at a
// time shares the same modem, so the two exclude each other's cycles.
RuntimeZteConfig zteTestCandidate;
volatile bool zteTestRunning = false;
volatile bool zteTestDone = false;
bool zteTestSuccess = false;
String zteTestMessage;
volatile bool ztePollCycleActive = false;
volatile bool ztePollStopRequested = false;

// One asynchronous ZTE SMS send at a time; started by an HTTP route with the
// recipient and body fields every SMS source will share, run on its own
// task, and excluded from poll/test cycles on the same modem.
String zteSendTo;
String zteSendText;
volatile bool zteSendRunning = false;
volatile bool zteSendDone = false;
bool zteSendSuccess = false;
String zteSendMessage;
TaskHandle_t ztePollTaskHandle = nullptr;
portMUX_TYPE zteStatusMux = portMUX_INITIALIZER_UNLOCKED;
char zteLastStatus[kZteStatusLength] = "";

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

// #region FUNC_startWallClock
// PURPOSE: Starts the bundled SNTP client whenever the station has an IP,
// because SEND_SMS validates sms_time against the modem's own synced clock;
// buildSmsTimeString keeps its placeholder until the first fix arrives.
void startWallClock() {
  configTime(0, 0, kNtpServers[0], kNtpServers[1]);
  Serial.printf("event=sntp_begin server=%s\n", kNtpServers[0]);
}
// #endregion FUNC_startWallClock

// #region FUNC_onStationConnected
// PURPOSE: Records healthy station connectivity and starts local name
// discovery.
void onStationConnected(bool deferAccessPointShutdown) {
  connectionState = ConnectionState::kOnline;
  nextReconnectAt = 0;
  lastConnectionError = "";
  startWallClock();
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

// #region FUNC_zteResultName
// PURPOSE: Maps a modem-dialog outcome onto the stable JSON token consumed
// by the UI.
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

// #region FUNC_publishZteStatus
// PURPOSE: Publishes one operator-facing poll status line for the web UI;
// the poll task is the only writer.
void publishZteStatus(const char* status) {
  portENTER_CRITICAL(&zteStatusMux);
  strncpy(zteLastStatus, status, kZteStatusLength - 1);
  zteLastStatus[kZteStatusLength - 1] = '\0';
  portEXIT_CRITICAL(&zteStatusMux);
}
// #endregion FUNC_publishZteStatus

// #region FUNC_readZteLastStatus
// PURPOSE: Snapshots the published poll status without holding the spinlock
// across String operations.
String readZteLastStatus() {
  portENTER_CRITICAL(&zteStatusMux);
  const String snapshot(zteLastStatus);
  portEXIT_CRITICAL(&zteStatusMux);
  return snapshot;
}
// #endregion FUNC_readZteLastStatus

// #region FUNC_sanitizeSenderForSubject
// PURPOSE: Reduces a sender field to printable ASCII so it can travel in an
// SMTP subject; control characters become spaces and others '?'.
String sanitizeSenderForSubject(const char* sender) {
  String clean;
  clean.reserve(strlen(sender));
  for (const char* p = sender; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 32 || ch > 126) {
      clean += ch < 32 ? ' ' : '?';
    } else {
      clean += *p;
    }
  }
  clean.trim();
  if (clean.length() == 0) {
    return F("unknown sender");
  }
  return clean.substring(0, 40);
}
// #endregion FUNC_sanitizeSenderForSubject

// #region FUNC_buildSmsEmail
// PURPOSE: Renders one SMS as the email subject and UTF-8 body, mirroring
// the reference forwarder's message layout.
void buildSmsEmail(const ZteSms& sms, String& subject, String& body) {
  const String sender = sanitizeSenderForSubject(sms.number);
  if (!sms.concatComplete) {
    const char* received = sms.concatReceived[0] != '\0' ? sms.concatReceived : "?";
    const char* total = sms.concatTotal[0] != '\0' ? sms.concatTotal : "?";
    subject = F("[INCOMPLETE ");
    subject += received;
    subject += '/';
    subject += total;
    subject += F("] SMS from ");
    subject += sender;
  } else {
    subject = F("SMS from ");
    subject += sender;
  }
  body = F("Sender: ");
  body += sender;
  if (storedZteConfig.label.length() > 0) {
    body += F("\nReceived on: ");
    body += storedZteConfig.label;
  }
  body += F("\nModem message ID: ");
  body += sms.id;
  body += F("\nModem date: ");
  char zteDate[64];
  formatZteDate(sms.dateRaw, zteDate, sizeof(zteDate));
  body += zteDate;
  body += F("\n\n");
  if (!sms.concatComplete) {
    body +=
        F("WARNING: modem received only part of this concatenated SMS; "
          "this email contains the available fragment.\n\n");
  }
  body += sms.textUtf8;
}
// #endregion FUNC_buildSmsEmail

// #region FUNC_forwardZteSms
// PURPOSE: Delivers one SMS through the stored SMTP profile; returns true
// only when the server accepted the message.
bool forwardZteSms(const ZteSms& sms) {
  const unsigned long startedAt = millis();
  Serial.printf("event=zte_forward_begin id=%s number=%s heap=%u\n", sms.id, sms.number,
                static_cast<unsigned>(ESP.getFreeHeap()));
  SecureSmtpChannel channel;
  SmtpClient client(channel);
  client.setStageListener(logSmtpStage);
  String subject;
  String body;
  buildSmsEmail(sms, subject, body);
  const SmtpConfigRecord record = buildSmtpConfigRecord(storedSmtpConfig);
  const SmtpSendResult result =
      client.sendMail(record, mdnsHostname.c_str(), subject.c_str(), body.c_str());
  Serial.printf(
      "event=zte_forward_result id=%s result=%s stage=%s code=%d elapsed_ms=%lu "
      "heap=%u\n",
      sms.id, smtpResultName(result), client.failedStage(), client.lastReplyCode(),
      millis() - startedAt, static_cast<unsigned>(ESP.getFreeHeap()));
  return result == SmtpSendResult::kSuccess;
}
// #endregion FUNC_forwardZteSms

// #region FUNC_runZtePollCycle
// PURPOSE: Performs one login-scan-forward-delete cycle against the modem;
// every outcome lands in a Serial event and the published status so a
// failing cycle is diagnosable from either interface.
void runZtePollCycle(ZteModem& modem) {
  Serial.printf("event=zte_poll_begin host=%s heap=%u\n", storedZteConfig.host.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
  ZteResult result = modem.login(storedZteConfig.host.c_str(), storedZteConfig.password.c_str());
  if (result != ZteResult::kSuccess) {
    publishZteStatus((String(F("Poll failed: ")) + modem.failedStage()).c_str());
    Serial.printf("event=zte_poll_complete result=login_failed stage=%s\n", modem.failedStage());
    return;
  }

  ZteSms* sms = new (std::nothrow) ZteSms();
  if (sms == nullptr) {
    publishZteStatus("Poll skipped: out of memory.");
    Serial.println("event=zte_poll_complete result=out_of_memory");
    return;
  }
  bool found = false;
  result = modem.findOldestIncoming(*sms, found);
  if (result != ZteResult::kSuccess) {
    publishZteStatus((String(F("Inbox scan failed: ")) + modem.failedStage()).c_str());
    Serial.printf("event=zte_poll_complete result=scan_failed stage=%s\n", modem.failedStage());
    delete sms;
    return;
  }
  if (!found) {
    publishZteStatus(
        (String(F("Connected to ")) + modem.waVersion() + F("; no incoming SMS.")).c_str());
    Serial.println("event=zte_poll_complete result=inbox_empty");
    delete sms;
    return;
  }

  Serial.printf("event=zte_sms_found id=%s complete=%s\n", sms->id,
                sms->concatComplete ? "true" : "false");
  if (!forwardZteSms(*sms)) {
    publishZteStatus("Email delivery failed; SMS kept on the modem.");
    Serial.printf("event=zte_poll_complete result=forward_failed id=%s\n", sms->id);
    delete sms;
    return;
  }
  result = modem.deleteSms(*sms);
  if (result == ZteResult::kSuccess) {
    Serial.printf("event=zte_delete_complete id=%s\n", sms->id);
    publishZteStatus((String(F("Forwarded SMS id=")) + sms->id + F(".")).c_str());
    Serial.printf("event=zte_poll_complete result=forwarded id=%s\n", sms->id);
  } else {
    Serial.printf("event=zte_delete_failed id=%s stage=%s result=%s\n", sms->id,
                  modem.failedStage(), zteResultName(result));
    publishZteStatus("SMS forwarded but deletion failed; it may be sent again.");
    Serial.printf("event=zte_poll_complete result=delete_unverified id=%s\n", sms->id);
  }
  delete sms;
}
// #endregion FUNC_runZtePollCycle

// #region FUNC_ztePollTask
// PURPOSE: Runs the poll lifecycle on its own task: waits for station
// connectivity, performs one cycle per interval in stoppable slices, and
// releases its heap before exiting.
void ztePollTask(void*) {
  char* scratch = static_cast<char*>(malloc(kZteScratchSize));
  NetworkZteChannel channel;
  ZteModem modem(channel, scratch, scratch == nullptr ? 0 : kZteScratchSize);
  bool waitingForStation = false;
  while (!ztePollStopRequested) {
    if (scratch == nullptr || WiFi.status() != WL_CONNECTED) {
      if (!waitingForStation) {
        waitingForStation = true;
        Serial.printf("event=zte_poll_wait reason=%s\n",
                      scratch == nullptr ? "out_of_memory" : "sta_down");
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    waitingForStation = false;
    if (zteTestRunning || zteSendRunning) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    ztePollCycleActive = true;
    runZtePollCycle(modem);
    ztePollCycleActive = false;
    for (unsigned long waited = 0; waited < kZtePollIntervalMs && !ztePollStopRequested;
         waited += 250) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
  free(scratch);
  Serial.println("event=zte_poll_stopped");
  ztePollTaskHandle = nullptr;
  vTaskDelete(nullptr);
}
// #endregion FUNC_ztePollTask

// #region FUNC_syncZtePollTask
// PURPOSE: Aligns the running poll task with the saved configuration; the
// task exists only when the source is enabled with credentials and an SMTP
// profile is configured.
void syncZtePollTask() {
  const bool shouldRun = zteConfigLoaded && storedZteConfig.enabled &&
                         storedZteConfig.host.length() > 0 && smtpConfigLoaded &&
                         storedSmtpConfig.host.length() > 0 &&
                         storedSmtpConfig.password.length() > 0;
  if (ztePollTaskHandle != nullptr) {
    ztePollStopRequested = true;
    const unsigned long deadline = millis() + 5000;
    while (ztePollTaskHandle != nullptr && millis() < deadline) {
      delay(10);
    }
    ztePollStopRequested = false;
    if (ztePollTaskHandle != nullptr) {
      Serial.println("event=zte_poll_stop_timeout");  // One leaked task; retry on next save.
      return;
    }
  }
  if (!shouldRun) {
    Serial.println("event=zte_poll_task state=stopped");
    return;
  }
  if (xTaskCreatePinnedToCore(ztePollTask, "zte_poll", 16384, nullptr, 1, &ztePollTaskHandle, 0) !=
      pdPASS) {
    ztePollTaskHandle = nullptr;
    Serial.println("event=zte_poll_task state=start_failed reason=task_create");
    return;
  }
  Serial.println("event=zte_poll_task state=started");
}
// #endregion FUNC_syncZtePollTask

// #region FUNC_zteTestTask
// PURPOSE: Runs the non-destructive connection test on its own task: LOGIN
// plus a capacity read, publishing the outcome last.
void zteTestTask(void*) {
  const RuntimeZteConfig candidate = zteTestCandidate;
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
  zteTestMessage = message;
  zteTestSuccess = result == ZteResult::kSuccess;
  Serial.printf("event=zte_test_complete result=%s stage=%s\n", zteResultName(result),
                modem.failedStage());
  zteTestRunning = false;
  zteTestDone = true;  // Published last: readers treat done as data-ready.
  vTaskDelete(nullptr);
}
// #endregion FUNC_zteTestTask

// #region FUNC_readZteForm
// PURPOSE: Validates the ZTE form into a runtime profile; an empty password
// keeps the stored one, matching the SMTP form contract.
bool readZteForm(RuntimeZteConfig& candidate, String& error) {
  candidate.enabled = server.arg("enabled") == F("1");
  candidate.host = server.arg("host");
  candidate.host.trim();
  if (candidate.host.length() == 0 || candidate.host.length() > kMaxZteHostLength ||
      !isPrintableAscii(candidate.host)) {
    error = F("Host must contain 1–63 printable ASCII characters.");
    return false;
  }
  candidate.password = server.arg("password");
  if (candidate.password.length() == 0) {
    if (!zteConfigLoaded || storedZteConfig.password.length() == 0) {
      error = F("Enter the modem web password.");
      return false;
    }
    candidate.password = storedZteConfig.password;
  } else if (candidate.password.length() > kMaxZtePasswordLength ||
             !isPrintableAscii(candidate.password)) {
    error = F("The modem web password must contain 1–63 printable ASCII characters.");
    return false;
  }
  candidate.label = server.arg("label");
  candidate.label.trim();
  if (candidate.label.length() > kMaxZteLabelLength || !isPrintableAscii(candidate.label)) {
    error = F("The phone number or alias must contain up to 31 printable ASCII characters.");
    return false;
  }
  return true;
}
// #endregion FUNC_readZteForm

// #region FUNC_buildWebZteConfig
// PURPOSE: Snapshots the stored ZTE profile for the JSON API without ever
// serializing the password.
WebZteConfig buildWebZteConfig() {
  WebZteConfig web;
  web.present = zteConfigLoaded && storedZteConfig.host.length() > 0;
  web.enabled = web.present && storedZteConfig.enabled;
  web.host = web.present ? storedZteConfig.host : String();
  web.passwordSet = web.present && storedZteConfig.password.length() > 0;
  web.label = web.present ? storedZteConfig.label : String();
  web.lastStatus = readZteLastStatus();
  return web;
}
// #endregion FUNC_buildWebZteConfig

// #region FUNC_handleZteConfigRequest
// PURPOSE: Returns the stored ZTE profile (without the password) for form
// prefill in the browser UI.
void handleZteConfigRequest() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server, 200, renderZteConfigJson(buildWebZteConfig()));
}
// #endregion FUNC_handleZteConfigRequest

// #region FUNC_handleZteSaveSubmission
// PURPOSE: Validates and persists the ZTE profile, then realigns the poll
// task with the new configuration.
void handleZteSaveSubmission() {
  Serial.println("event=http_zte_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeZteConfig candidate;
  String error;
  if (!readZteForm(candidate, error)) {
    sendJsonError(400, error);
    return;
  }
  if (!zteConfigStore.save(candidate)) {
    sendJsonError(500, F("The ZTE configuration could not be saved."));
    return;
  }
  storedZteConfig = candidate;
  zteConfigLoaded = true;
  zteTestDone = false;
  zteTestMessage = "";
  syncZtePollTask();
  Serial.println("event=zte_saved");
  sendJson(server, 200, renderMessageJson(F("ZTE settings saved.")));
}
// #endregion FUNC_handleZteSaveSubmission

// #region FUNC_handleZteTestStart
// PURPOSE: Starts one non-destructive connection test on a dedicated task
// so loop() and the HTTP server stay responsive during the dialog.
void handleZteTestStart() {
  Serial.println("event=http_zte_test_submit");
  if (!requireAuthentication()) {
    return;
  }
  if (zteTestRunning) {
    sendJsonError(409, F("A connection test is already in progress."));
    return;
  }
  if (zteSendRunning) {
    sendJsonError(409, F("An SMS send is in progress; try again in a few seconds."));
    return;
  }
  if (ztePollCycleActive) {
    sendJsonError(409, F("A poll cycle is in progress; try again in a few seconds."));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(400, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  RuntimeZteConfig candidate;
  String error;
  if (!readZteForm(candidate, error)) {
    sendJsonError(400, error);
    return;
  }
  zteTestCandidate = candidate;
  zteTestDone = false;
  zteTestMessage = "";
  zteTestRunning = true;
  if (xTaskCreatePinnedToCore(zteTestTask, "zte_test", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    zteTestRunning = false;
    Serial.println("event=zte_test_failed reason=task_create");
    sendJsonError(503, F("The test could not be started. Try again."));
    return;
  }
  sendJson(server, 200, renderMessageJson(F("Connection test started.")));
}
// #endregion FUNC_handleZteTestStart

// #region FUNC_handleZteTestStatus
// PURPOSE: Reports asynchronous test progress to the polling browser UI.
void handleZteTestStatus() {
  if (!requireAuthentication()) {
    return;
  }
  WebAsyncOp op;
  op.running = zteTestRunning;
  op.done = zteTestDone;
  if (zteTestDone) {
    op.result = zteTestSuccess ? "success" : "failed";
    op.message = zteTestMessage;
  }
  sendJson(server, 200, renderAsyncOpJson(op));
}
// #endregion FUNC_handleZteTestStatus

// #region FUNC_isValidZteSmsRecipient
// PURPOSE: Accepts only an optional leading plus followed by digits so the
// Number field can never smuggle form separators into the goform request.
bool isValidZteSmsRecipient(const String& value) {
  if (value.length() < 3 || value.length() > 20) {
    return false;
  }
  for (size_t index = 0; index < value.length(); ++index) {
    const char ch = value[index];
    if (ch == '+' && index == 0) {
      continue;
    }
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}
// #endregion FUNC_isValidZteSmsRecipient

// #region FUNC_readZteSendForm
// PURPOSE: Validates the outgoing-SMS form (recipient and body) every SMS
// source will share; the body rule is the modem's own UNICODE send limit in
// UTF-16 code units, so emoji and Cyrillic are counted like the modem does.
bool readZteSendForm(String& to, String& text, String& error) {
  to = server.arg("to");
  to.trim();
  if (!isValidZteSmsRecipient(to)) {
    error = F("Recipient must be 3\u201320 digits with an optional leading +.");
    return false;
  }
  text = server.arg("text");
  const size_t units = zteSmsUtf16Units(text.c_str());
  if (units == kZteSmsInvalidUnits) {
    error = F("The message is not valid UTF-8 text.");
    return false;
  }
  if (units == 0) {
    error = F("Enter the message text.");
    return false;
  }
  if (units > kMaxZteSmsSendUnits) {
    error = F("The message is too long; the modem accepts at most 335 characters.");
    return false;
  }
  return true;
}
// #endregion FUNC_readZteSendForm

// #region FUNC_zteReplySnippet
// PURPOSE: Reduces one modem reply to a short printable-ASCII snippet for
// Serial events and the UI; never contains credentials (goform replies hold
// only result codes and counters).
String zteReplySnippet(const char* body) {
  String snippet;
  for (const char* p = body; *p != '\0' && snippet.length() < 96; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    snippet += (ch >= 32 && ch <= 126) ? static_cast<char>(ch) : '.';
  }
  return snippet;
}
// #endregion FUNC_zteReplySnippet

// #region FUNC_zteSendTask
// PURPOSE: Runs one send on its own task: LOGIN, SEND_SMS, then bounded
// one-per-second status samples until the modem completes or fails the
// command, publishing the outcome last.
void zteSendTask(void*) {
  const String to = zteSendTo;
  const String text = zteSendText;
  const unsigned long startedAt = millis();
  Serial.printf("event=zte_send_begin to=%s units=%u epoch=%ld heap=%u\n", to.c_str(),
                static_cast<unsigned>(zteSmsUtf16Units(text.c_str())),
                static_cast<long>(time(nullptr)), static_cast<unsigned>(ESP.getFreeHeap()));

  char* scratch = static_cast<char*>(malloc(kZteScratchSize));
  NetworkZteChannel channel;
  ZteModem modem(channel, scratch, scratch == nullptr ? 0 : kZteScratchSize);
  ZteResult result = scratch == nullptr ? ZteResult::kProtocolError
                                        : modem.login(storedZteConfig.host.c_str(),
                                                      storedZteConfig.password.c_str());
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
        result = pollResult;  // Transport failed mid-send; the SMS may still deliver.
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
  free(scratch);

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
    // Accepted, but the bounded status wait ended without a terminal sample.
    message = F("The modem accepted the message but its status stayed in progress; ");
    message += F("it may still be delivered. [stage=send_status]");
  } else if (result == ZteResult::kSendRejected) {
    message = F("The modem rejected the message. Modem reply: ");
    message += zteReplySnippet(modem.lastBody());
  } else {
    message = F("Send failed [stage=");
    message += modem.failedStage();
    message += ']';
  }
  zteSendMessage = message;
  zteSendSuccess = confirmed;
  if (!confirmed) {
    // Byte-level diagnosis: the exact request the modem rejected (holds no
    // credentials; AD is an anti-CSRF token, the number/text are the
    // operator's own message).
    Serial.printf("event=zte_send_form form=%s\n", modem.lastSendForm());
  }
  const String replyDetail =
      result == ZteResult::kSendRejected ? zteReplySnippet(modem.lastBody()) : String();
  Serial.printf(
      "event=zte_send_complete result=%s confirmed=%s stage=%s elapsed_ms=%lu detail=%s\n",
      zteResultName(result), confirmed ? "true" : "false", modem.failedStage(),
      millis() - startedAt, replyDetail.c_str());
  zteSendRunning = false;
  zteSendDone = true;  // Published last: readers treat done as data-ready.
  vTaskDelete(nullptr);
}
// #endregion FUNC_zteSendTask

// #region FUNC_handleZteSendStart
// PURPOSE: Starts one outgoing SMS on a dedicated task so loop() and the
// HTTP server stay responsive during the modem dialog.
void handleZteSendStart() {
  Serial.println("event=http_zte_send_submit");
  if (!requireAuthentication()) {
    return;
  }
  if (zteSendRunning) {
    sendJsonError(409, F("An SMS send is already in progress."));
    return;
  }
  if (zteTestRunning) {
    sendJsonError(409, F("A connection test is in progress; try again in a few seconds."));
    return;
  }
  if (ztePollCycleActive) {
    sendJsonError(409, F("A poll cycle is in progress; try again in a few seconds."));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(400, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  if (!zteConfigLoaded || storedZteConfig.host.length() == 0 ||
      storedZteConfig.password.length() == 0) {
    sendJsonError(400, F("Save the ZTE modem settings (host and password) before sending."));
    return;
  }
  // SEND_SMS validates sms_time; without an SNTP fix the modem rejects it.
  if (time(nullptr) < 1577836800) {  // Before 2020-01-01: clock not synced yet.
    sendJsonError(503, F("Waiting for the internet time sync; try again in a minute."));
    return;
  }
  String to;
  String text;
  String error;
  if (!readZteSendForm(to, text, error)) {
    sendJsonError(400, error);
    return;
  }
  zteSendTo = to;
  zteSendText = text;
  zteSendDone = false;
  zteSendMessage = "";
  zteSendRunning = true;
  if (xTaskCreatePinnedToCore(zteSendTask, "zte_send", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    zteSendRunning = false;
    Serial.println("event=zte_send_failed reason=task_create");
    sendJsonError(503, F("The send could not be started. Try again."));
    return;
  }
  sendJson(server, 200, renderMessageJson(F("Send started.")));
}
// #endregion FUNC_handleZteSendStart

// #region FUNC_handleZteSendStatus
// PURPOSE: Reports asynchronous send progress to the polling browser UI.
void handleZteSendStatus() {
  if (!requireAuthentication()) {
    return;
  }
  WebAsyncOp op;
  op.running = zteSendRunning;
  op.done = zteSendDone;
  if (zteSendDone) {
    op.result = zteSendSuccess ? "success" : "failed";
    op.message = zteSendMessage;
  }
  sendJson(server, 200, renderAsyncOpJson(op));
}
// #endregion FUNC_handleZteSendStatus

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
  syncZtePollTask();
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
  WebAsyncOp op;
  op.running = smtpTestRunning;
  op.done = smtpTestDone;
  if (smtpTestDone) {
    op.result = smtpResultName(smtpTestResult);
    op.message = smtpTestMessage;
  }
  sendJson(server, 200, renderAsyncOpJson(op));
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
  server.on("/api/zte", HTTP_GET, handleZteConfigRequest);
  server.on("/api/zte", HTTP_POST, handleZteSaveSubmission);
  server.on("/api/zte/test", HTTP_POST, handleZteTestStart);
  server.on("/api/zte/test", HTTP_GET, handleZteTestStatus);
  server.on("/api/zte/send", HTTP_POST, handleZteSendStart);
  server.on("/api/zte/send", HTTP_GET, handleZteSendStatus);
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
  zteConfigLoaded = zteConfigStore.load(storedZteConfig);
  recordBootStage(String(F("event=boot_zte_config_loaded present=")) +
                  (zteConfigLoaded ? String(storedZteConfig.enabled ? F("true enabled=true")
                                                                    : F("true enabled=false"))
                                   : F("false")));

  recordBootStage(F("event=boot_http_routes_begin"));
  configureWebServer();
  syncZtePollTask();
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
