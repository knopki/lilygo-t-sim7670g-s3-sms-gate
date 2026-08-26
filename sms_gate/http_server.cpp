// #region MODULE_CONTRACT
// PURPOSE: Implements HttpServer so the sketch only drives
// setupFirmware/loopFirmware while all HTTP handlers live here.
// #endregion MODULE_CONTRACT
#include "http_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include "config_store_common.h"
#include "sms_validate.h"
#include "web_api.h"
namespace {
constexpr char kAdminUser[] = "admin";
constexpr char kAuthRealm[] = "SMS Gate";
constexpr int kHttpOk = 200;
constexpr int kHttpRedirect = 302;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpForbidden = 403;
constexpr int kHttpNotFound = 404;
constexpr int kHttpConflict = 409;
constexpr int kHttpInternalError = 500;
constexpr int kHttpUnavailable = 503;
}  // namespace
// #region CLASS_HttpServer
// #region METHOD_constructor
// PURPOSE: Binds shared services by reference.
HttpServer::HttpServer(WebServer& server, ConfigStore& store, RuntimeConfig& config,
                       WifiManager& wifi, SmtpService& smtp, ZteService& zte, ModemService& modem)
    : server_(server),
      configStore_(store),
      config_(config),
      wifi_(wifi),
      smtp_(smtp),
      zte_(zte),
      modem_(modem) {}
// #endregion METHOD_constructor
// #region METHOD_requireAuthentication
// PURPOSE: Guards protected routes with Digest authentication.
bool HttpServer::requireAuthentication() {
  if (server_.authenticate(kAdminUser, config_.adminPassword.c_str())) {
    return true;
  }
  Serial.println("event=http_auth_rejected");
  server_.requestAuthentication(DIGEST_AUTH, kAuthRealm);
  return false;
}
// #endregion METHOD_requireAuthentication
// #region METHOD_sendJsonError
// PURPOSE: Emits the uniform error envelope.
void HttpServer::sendJsonError(int code, const String& error) {
  sendJson(server_, code, renderErrorJson(error));
}
// #endregion METHOD_sendJsonError
// #region METHOD_readCandidateConfig
// PURPOSE: Validates form data before a station connection test.
bool HttpServer::readCandidateConfig(RuntimeConfig& candidate, String& error) {
  candidate.ssid = server_.arg("ssid");
  candidate.wifiPassword = server_.arg("wifi_password");
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
// #endregion METHOD_readCandidateConfig
// #region METHOD_checkCommonSendBusy
// PURPOSE: SendGate common busy guard.
bool HttpServer::checkCommonSendBusy(String& error, int& code) {
  if (zte_.isSendRunning() || modem_.isSendRunning()) {
    error = F("An SMS send is already in progress.");
    code = kHttpConflict;
    return false;
  }
  if (zte_.isTestRunning()) {
    error = F("A connection test is in progress; try again in a few seconds.");
    code = kHttpConflict;
    return false;
  }
  if (zte_.isPollCycleActive() || modem_.isPollCycleActive()) {
    error = F("A poll cycle is in progress; try again in a few seconds.");
    code = kHttpConflict;
    return false;
  }
  return true;
}
// #endregion METHOD_checkCommonSendBusy
// #region METHOD_mapModemSendErrorToHttpCode
// PURPOSE: Centralizes string-to-code mapping for modem send failures so
// handleModemSendStart and handleSmsSendStart share one predicate.
int HttpServer::mapModemSendErrorToHttpCode(const String& error) const {
  if (error.indexOf(F("already in progress")) >= 0 || error.indexOf(F("poll cycle")) >= 0) {
    return kHttpConflict;
  }
  if (error.indexOf(F("not responding")) >= 0 || error.indexOf(F("SIM card")) >= 0) {
    return kHttpUnavailable;
  }
  if (error.indexOf(F("not connected")) >= 0) {
    return kHttpBadRequest;
  }
  if (error.indexOf(F("Recipient")) >= 0 || error.indexOf(F("valid UTF-8")) >= 0) {
    return kHttpBadRequest;
  }
  return kHttpUnavailable;
}
// #endregion METHOD_mapModemSendErrorToHttpCode
// #region METHOD_ensureModemReadyForSend
// PURPOSE: Shared WiFi/modem readiness check before any AT send.
bool HttpServer::ensureModemReadyForSend(String& error, int& code) const {
  if (WiFi.status() != WL_CONNECTED) {
    error = F("The device is not connected to a Wi-Fi network.");
    code = kHttpBadRequest;
    return false;
  }
  const ModemStatus snapshot = modem_.readStatus();
  if (!snapshot.present) {
    error = F("The internal modem is not responding; try again.");
    code = kHttpUnavailable;
    return false;
  }
  if (strcmp(snapshot.cpin, "READY") != 0) {
    error = F("The SIM card is not ready; check the modem status.");
    code = kHttpUnavailable;
    return false;
  }
  return true;
}
// #endregion METHOD_ensureModemReadyForSend
// #region METHOD_handleModemViaSend
// PURPOSE: Executes modem via path with busy/WiFi/CPIN guards and unified error mapping.
bool HttpServer::handleModemViaSend(const String& to, const String& text) {
  String busyError;
  int busyCode = kHttpConflict;
  if (!checkCommonSendBusy(busyError, busyCode)) {
    sendJsonError(busyCode, busyError);
    return true;
  }
  String readyError;
  int readyCode = kHttpUnavailable;
  if (!ensureModemReadyForSend(readyError, readyCode)) {
    sendJsonError(readyCode, readyError);
    return true;
  }
  String sendError;
  if (!modem_.startSend(to, text, sendError)) {
    sendJsonError(mapModemSendErrorToHttpCode(sendError), sendError);
    return true;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Send started.")));
  return true;
}
// #endregion METHOD_handleModemViaSend
// #region METHOD_handleZteViaSend
// PURPOSE: Executes ZTE via path with common guards and ZTE-loaded check.
bool HttpServer::handleZteViaSend(const String& to, const String& text) {
  String busyError;
  int busyCode = kHttpConflict;
  if (!checkCommonSendBusy(busyError, busyCode)) {
    sendJsonError(busyCode, busyError);
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(kHttpBadRequest, F("The device is not connected to a Wi-Fi network."));
    return true;
  }
  if (!zte_.isLoaded() || zte_.config().host.length() == 0 ||
      zte_.config().password.length() == 0) {
    sendJsonError(kHttpBadRequest,
                  F("Save the ZTE modem settings (host and password) before sending."));
    return true;
  }
  String sendError;
  if (!zte_.startSend(to, text, sendError)) {
    sendJsonError(kHttpUnavailable, sendError);
    return true;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Send started.")));
  return true;
}
// #endregion METHOD_handleZteViaSend
// #region METHOD_handleStatusRequest
// PURPOSE: Reports controller state as JSON.
void HttpServer::handleStatusRequest() {
  if (config_.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderStatusJson(wifi_.buildStatus(config_)));
}
// #endregion METHOD_handleStatusRequest
// #region METHOD_handleScanRequest
// PURPOSE: Returns scanned networks as JSON.
void HttpServer::handleScanRequest() {
  if (config_.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, wifi_.buildScanNetworksJson());
}
// #endregion METHOD_handleScanRequest
// #region METHOD_handleSetupSubmission
// PURPOSE: Accepts initial configuration after Wi-Fi test.
void HttpServer::handleSetupSubmission() {
  Serial.println("event=http_setup_submit");
  if (config_.ssid.length() > 0) {
    sendJsonError(kHttpForbidden, F("Initial setup is already complete."));
    return;
  }
  RuntimeConfig candidate;
  String error;
  if (!readCandidateConfig(candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  candidate.adminPassword = server_.arg("admin_password");
  const String confirmation = server_.arg("admin_password_confirm");
  if (!isValidPassword(candidate.adminPassword) ||
      !constantTimeEquals(candidate.adminPassword, confirmation)) {
    sendJsonError(
        kHttpBadRequest,
        F("Administrator passwords must match and contain 8–63 printable ASCII characters."));
    return;
  }
  const RuntimeConfig previousSetupConfig = config_;
  if (!wifi_.testStationCandidate(candidate, previousSetupConfig)) {
    wifi_.setLastConnectionError(
        F("Could not connect with those Wi-Fi credentials. Nothing was saved."));
    sendJsonError(kHttpBadRequest, wifi_.lastConnectionError());
    return;
  }
  if (!configStore_.save(candidate)) {
    wifi_.setLastConnectionError(F("Configuration could not be saved."));
    sendJsonError(kHttpInternalError, wifi_.lastConnectionError());
    return;
  }
  config_ = candidate;
  wifi_.onStationConnected(true);
  String message = F("Configuration saved. The access point will close shortly. Open http://");
  message += wifi_.mdnsHostname();
  message += F(".local on the configured network.");
  sendJson(server_, kHttpOk, renderMessageJson(message));
}
// #endregion METHOD_handleSetupSubmission
// #region METHOD_handleNetworkSubmission
// PURPOSE: Replaces saved profile only after it connects.
void HttpServer::handleNetworkSubmission() {
  Serial.println("event=http_network_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeConfig candidate = config_;
  String error;
  if (!readCandidateConfig(candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  const RuntimeConfig previousNetworkConfig = config_;
  if (!wifi_.testStationCandidate(candidate, previousNetworkConfig)) {
    wifi_.setLastConnectionError(
        F("Could not connect with those Wi-Fi credentials. "
          "The previous profile was kept."));
    sendJsonError(kHttpBadRequest, wifi_.lastConnectionError());
    return;
  }
  if (!configStore_.save(candidate)) {
    wifi_.setLastConnectionError(F("Configuration could not be saved."));
    sendJsonError(kHttpInternalError, wifi_.lastConnectionError());
    return;
  }
  config_ = candidate;
  wifi_.onStationConnected(true);
  String message = F("Configuration saved. The interface is now available at http://");
  message += wifi_.mdnsHostname();
  message += F(".local.");
  sendJson(server_, kHttpOk, renderMessageJson(message));
}
// #endregion METHOD_handleNetworkSubmission
// #region METHOD_handlePasswordSubmission
// PURPOSE: Changes admin/fallback-AP password.
void HttpServer::handlePasswordSubmission() {
  Serial.println("event=http_password_submit");
  if (!requireAuthentication()) {
    return;
  }
  const String currentPassword = server_.arg("current_password");
  const String newPassword = server_.arg("new_password");
  const String confirmation = server_.arg("new_password_confirm");
  if (!constantTimeEquals(currentPassword, config_.adminPassword)) {
    sendJsonError(kHttpBadRequest, F("The current administrator password is incorrect."));
    return;
  }
  if (!isValidPassword(newPassword) || !constantTimeEquals(newPassword, confirmation)) {
    sendJsonError(kHttpBadRequest,
                  F("New passwords must match and contain 8–63 printable ASCII characters."));
    return;
  }
  RuntimeConfig candidate = config_;
  candidate.adminPassword = newPassword;
  if (!configStore_.save(candidate)) {
    wifi_.setLastConnectionError(F("Configuration could not be saved."));
    sendJsonError(kHttpInternalError, wifi_.lastConnectionError());
    return;
  }
  config_ = candidate;
  wifi_.scheduleAccessPointRestart();
  sendJson(server_, kHttpOk,
           renderMessageJson(F("Password changed. The browser will ask for the new password on "
                               "the next request.")));
}
// #endregion METHOD_handlePasswordSubmission
// #region METHOD_handleSmtpConfigRequest
// PURPOSE: Returns stored SMTP profile without password.
void HttpServer::handleSmtpConfigRequest() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderSmtpConfigJson(smtp_.webConfig()));
}
// #endregion METHOD_handleSmtpConfigRequest
// #region METHOD_handleSmtpSaveSubmission
// PURPOSE: Validates and persists SMTP profile.
void HttpServer::handleSmtpSaveSubmission() {
  Serial.println("event=http_smtp_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeSmtpConfig candidate;
  String error;
  if (!smtp_.readSmtpForm(server_, candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  if (!smtp_.save(candidate)) {
    sendJsonError(kHttpInternalError, F("The SMTP configuration could not be saved."));
    return;
  }
  {
    const bool smtpReady = smtp_.isLoaded() && smtp_.config().host.length() > 0 &&
                           smtp_.config().password.length() > 0;
    zte_.syncPollTask(zte_.shouldRunPoll(smtpReady));
  }
  Serial.println("event=smtp_saved");
  sendJson(server_, kHttpOk,
           renderMessageJson(F("SMTP settings saved. Use the test button to verify delivery.")));
}
// #endregion METHOD_handleSmtpSaveSubmission
// #region METHOD_handleSmtpTestStart
// PURPOSE: Starts one test delivery on its own task.
void HttpServer::handleSmtpTestStart() {
  Serial.println("event=http_smtp_test_submit");
  if (!requireAuthentication()) {
    return;
  }
  if (smtp_.isTestRunning()) {
    sendJsonError(kHttpConflict, F("A test delivery is already in progress."));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(kHttpBadRequest, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  RuntimeSmtpConfig candidate;
  String error;
  if (!smtp_.readSmtpForm(server_, candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  String testError;
  if (!smtp_.startTest(candidate, wifi_.mdnsHostname() + F(".local"), testError)) {
    sendJsonError(kHttpUnavailable, testError);
    return;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Test delivery started.")));
}
// #endregion METHOD_handleSmtpTestStart
// #region METHOD_handleSmtpTestStatus
// PURPOSE: Reports async test progress.
void HttpServer::handleSmtpTestStatus() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderAsyncOpJson(smtp_.testStatus()));
}
// #endregion METHOD_handleSmtpTestStatus
// #region METHOD_handleZteConfigRequest
// PURPOSE: Returns stored ZTE profile without password.
void HttpServer::handleZteConfigRequest() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderZteConfigJson(zte_.webConfig()));
}
// #endregion METHOD_handleZteConfigRequest
// #region METHOD_handleZteSaveSubmission
// PURPOSE: Validates and persists ZTE profile.
void HttpServer::handleZteSaveSubmission() {
  Serial.println("event=http_zte_submit");
  if (!requireAuthentication()) {
    return;
  }
  RuntimeZteConfig candidate;
  String error;
  if (!zte_.readForm(server_, candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  if (!zte_.save(candidate)) {
    sendJsonError(kHttpInternalError, F("The ZTE configuration could not be saved."));
    return;
  }
  {
    const bool smtpReady = smtp_.isLoaded() && smtp_.config().host.length() > 0 &&
                           smtp_.config().password.length() > 0;
    zte_.syncPollTask(zte_.shouldRunPoll(smtpReady));
  }
  Serial.printf("event=zte_saved enabled=%s poll_interval=%u\n",
                candidate.enabled ? "true" : "false",
                static_cast<unsigned>(candidate.pollIntervalSec));
  sendJson(server_, kHttpOk, renderMessageJson(F("ZTE settings saved.")));
}
// #endregion METHOD_handleZteSaveSubmission
// #region METHOD_handleZteTestStart
// PURPOSE: Starts one non-destructive ZTE connection test.
void HttpServer::handleZteTestStart() {
  Serial.println("event=http_zte_test_submit");
  if (!requireAuthentication()) {
    return;
  }
  if (zte_.isTestRunning()) {
    sendJsonError(kHttpConflict, F("A connection test is already in progress."));
    return;
  }
  if (zte_.isSendRunning() || modem_.isSendRunning()) {
    sendJsonError(kHttpConflict, F("An SMS send is in progress; try again in a few seconds."));
    return;
  }
  if (zte_.isPollCycleActive() || modem_.isPollCycleActive()) {
    sendJsonError(kHttpConflict, F("A poll cycle is in progress; try again in a few seconds."));
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(kHttpBadRequest, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  RuntimeZteConfig candidate;
  String error;
  if (!zte_.readForm(server_, candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  String testError;
  if (!zte_.startTest(candidate, testError)) {
    sendJsonError(kHttpUnavailable, testError);
    return;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Connection test started.")));
}
// #endregion METHOD_handleZteTestStart
// #region METHOD_handleZteTestStatus
// PURPOSE: Reports ZTE test progress.
void HttpServer::handleZteTestStatus() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderAsyncOpJson(zte_.testStatus()));
}
// #endregion METHOD_handleZteTestStatus
// #region METHOD_handleZteSendStart
// PURPOSE: Starts one ZTE outgoing SMS.
void HttpServer::handleZteSendStart() {
  Serial.println("event=http_zte_send_submit");
  if (!requireAuthentication()) {
    return;
  }
  String busyError;
  int busyCode = kHttpConflict;
  if (!checkCommonSendBusy(busyError, busyCode)) {
    sendJsonError(busyCode, busyError);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendJsonError(kHttpBadRequest, F("The device is not connected to a Wi-Fi network."));
    return;
  }
  if (!zte_.isLoaded() || zte_.config().host.length() == 0 ||
      zte_.config().password.length() == 0) {
    sendJsonError(kHttpBadRequest,
                  F("Save the ZTE modem settings (host and password) before sending."));
    return;
  }
  String to;
  String text;
  String error;
  if (!zte_.readSendForm(server_, to, text, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  String sendError;
  if (!zte_.startSend(to, text, sendError)) {
    sendJsonError(kHttpUnavailable, sendError);
    return;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Send started.")));
}
// #endregion METHOD_handleZteSendStart
// #region METHOD_handleZteSendStatus
// PURPOSE: Reports ZTE send progress.
void HttpServer::handleZteSendStatus() {
  if (!requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderAsyncOpJson(zte_.sendStatus()));
}
// #endregion METHOD_handleZteSendStatus
// #region METHOD_handleModemStatusRequest
// PURPOSE: Serves GET /api/modem/status.
void HttpServer::handleModemStatusRequest() {
  if (config_.ssid.length() > 0 && !requireAuthentication()) return;
  sendJson(server_, kHttpOk, renderModemStatusJson(modem_.webStatus()));
}
// #endregion METHOD_handleModemStatusRequest
// #region METHOD_handleModemSourceRequest
// PURPOSE: Returns modem-source profile.
void HttpServer::handleModemSourceRequest() {
  if (config_.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  sendJson(server_, kHttpOk, renderModemSourceJson(modem_.webSourceConfig()));
}
// #endregion METHOD_handleModemSourceRequest
// #region METHOD_handleModemSourceSave
// PURPOSE: Validates and persists modem-source profile.
void HttpServer::handleModemSourceSave() {
  Serial.println("event=http_modem_source_submit");
  if (config_.ssid.length() > 0 && !requireAuthentication()) {
    return;
  }
  RuntimeModemSourceConfig candidate;
  String error;
  if (!modem_.readSourceForm(server_, candidate, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  if (!modem_.save(candidate)) {
    sendJsonError(kHttpInternalError, F("The modem source configuration could not be saved."));
    return;
  }
  Serial.printf("event=modem_source_saved enabled=%s poll_interval=%u\n",
                candidate.enabled ? "true" : "false",
                static_cast<unsigned>(candidate.pollIntervalSec));
  sendJson(server_, kHttpOk, renderMessageJson(F("Modem source settings saved.")));
}
// #endregion METHOD_handleModemSourceSave
// #region METHOD_handleModemSendStart
// PURPOSE: Starts one SIM7670G outgoing SMS.
void HttpServer::handleModemSendStart() {
  Serial.println("event=http_modem_send_submit");
  if (!requireAuthentication()) return;
  String readyError;
  int readyCode = kHttpUnavailable;
  // Validate form before readiness so form errors (400) surface before 503.
  String to;
  String text;
  String formError;
  if (!zte_.readSendForm(server_, to, text, formError)) {
    sendJsonError(kHttpBadRequest, formError);
    return;
  }
  String busyError;
  int busyCode = kHttpConflict;
  if (!checkCommonSendBusy(busyError, busyCode)) {
    sendJsonError(busyCode, busyError);
    return;
  }
  if (!ensureModemReadyForSend(readyError, readyCode)) {
    sendJsonError(readyCode, readyError);
    return;
  }
  String sendError;
  if (!modem_.startSend(to, text, sendError)) {
    sendJsonError(mapModemSendErrorToHttpCode(sendError), sendError);
    return;
  }
  sendJson(server_, kHttpOk, renderMessageJson(F("Send started.")));
}
// #endregion METHOD_handleModemSendStart
// #region METHOD_handleModemSendStatus
// PURPOSE: Reports modem send progress.
void HttpServer::handleModemSendStatus() {
  if (!requireAuthentication()) return;
  sendJson(server_, kHttpOk, renderAsyncOpJson(modem_.sendStatus()));
}
// #endregion METHOD_handleModemSendStatus
// #region METHOD_handleSmsSendStart
// PURPOSE: Unified send entry point POST /api/sms/send.
void HttpServer::handleSmsSendStart() {
  Serial.println("event=http_sms_send_submit");
  if (!requireAuthentication()) return;
  String via = server_.arg("via");
  via.trim();
  via.toLowerCase();
  if (via.length() == 0) via = "zte";
  if (via != "zte" && via != "modem") {
    sendJsonError(kHttpBadRequest, F("Field via must be \"zte\" or \"modem\"."));
    return;
  }
  String to;
  String text;
  String error;
  if (!zte_.readSendForm(server_, to, text, error)) {
    sendJsonError(kHttpBadRequest, error);
    return;
  }
  if (via == "modem") {
    handleModemViaSend(to, text);
    return;
  }
  handleZteViaSend(to, text);
}
// #endregion METHOD_handleSmsSendStart
// #region METHOD_handleNotFound
// PURPOSE: Redirects captive-portal requests or returns 404.
void HttpServer::handleNotFound() {
  if (wifi_.accessPointActive()) {
    server_.sendHeader("Location", "/");
    server_.send(kHttpRedirect, "text/plain", "Redirecting to configuration.");
    return;
  }
  server_.send(kHttpNotFound, "text/plain", "Not found.");
}
// #endregion METHOD_handleNotFound
// #region METHOD_begin
// PURPOSE: Registers routes and starts the server.
void HttpServer::begin() {
  Serial.println("event=http_routes_register_begin");
  server_.on("/", HTTP_GET, [this]() { sendAsset(server_, "/"); });
  server_.on("/app.js", HTTP_GET, [this]() { sendAsset(server_, "/app.js"); });
  server_.on("/style.css", HTTP_GET, [this]() { sendAsset(server_, "/style.css"); });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatusRequest(); });
  server_.on("/api/scan", HTTP_GET, [this]() { handleScanRequest(); });
  server_.on("/api/setup", HTTP_POST, [this]() { handleSetupSubmission(); });
  server_.on("/api/network", HTTP_POST, [this]() { handleNetworkSubmission(); });
  server_.on("/api/password", HTTP_POST, [this]() { handlePasswordSubmission(); });
  server_.on("/api/smtp", HTTP_GET, [this]() { handleSmtpConfigRequest(); });
  server_.on("/api/smtp", HTTP_POST, [this]() { handleSmtpSaveSubmission(); });
  server_.on("/api/smtp/test", HTTP_POST, [this]() { handleSmtpTestStart(); });
  server_.on("/api/smtp/test", HTTP_GET, [this]() { handleSmtpTestStatus(); });
  server_.on("/api/zte", HTTP_GET, [this]() { handleZteConfigRequest(); });
  server_.on("/api/zte", HTTP_POST, [this]() { handleZteSaveSubmission(); });
  server_.on("/api/zte/test", HTTP_POST, [this]() { handleZteTestStart(); });
  server_.on("/api/zte/test", HTTP_GET, [this]() { handleZteTestStatus(); });
  server_.on("/api/zte/send", HTTP_POST, [this]() { handleZteSendStart(); });
  server_.on("/api/zte/send", HTTP_GET, [this]() { handleZteSendStatus(); });
  server_.on("/api/modem/send", HTTP_POST, [this]() { handleModemSendStart(); });
  server_.on("/api/modem/send", HTTP_GET, [this]() { handleModemSendStatus(); });
  server_.on("/api/sms/send", HTTP_POST, [this]() { handleSmsSendStart(); });
  server_.on("/api/modem/status", HTTP_GET, [this]() { handleModemStatusRequest(); });
  server_.on("/api/modem/source", HTTP_GET, [this]() { handleModemSourceRequest(); });
  server_.on("/api/modem/source", HTTP_POST, [this]() { handleModemSourceSave(); });
  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
  Serial.printf("event=http_server_started port=%u\n", kHttpPort);
}
// #endregion METHOD_begin
// #endregion CLASS_HttpServer
