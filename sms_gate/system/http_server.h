// #region MODULE_CONTRACT
// PURPOSE: Keeps authentication and route policy in one boundary.
// SCOPE:
// - Registers protected pages, provisioning/configuration APIs, SMS
// send/test routes, captive-portal redirects, and JSON/assets.
// - NOT: service lifecycles, persistence, modem dialogs, SMTP/TLS, or email rendering.
// INVARIANTS:
// - Responses never expose credentials;
// - validation errors use JSON;
// - busy sends return 409 and unavailable modems return 503.
// DEPENDENCIES: WebServer/WiFi; ConfigStore, WifiManager, SmtpService,
// ZteService, ModemService, GpsService, TimeSync, and web_api.h.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_HTTP_SERVER_H
#define SYSTEM_HTTP_SERVER_H

#include <Arduino.h>
#include <WebServer.h>

#include "persistence/config_store.h"
#include "gps/gps_service.h"
#include "modem/modem_service.h"
#include "smtp/smtp_service.h"
#include "system/time_sync.h"
#include "system/wifi_manager.h"
#include "zte/zte_service.h"

// #region CLASS_HttpServer
// PURPOSE: Encapsulates all HTTP route registration and handlers so
// sms_gate.ino only constructs services and calls begin/handleClient.
class HttpServer {
 public:
  HttpServer(WebServer& server, ConfigStore& store, RuntimeConfig& config, WifiManager& wifi,
             SmtpService& smtp, ZteService& zte, ModemService& modem, GpsService& gps,
             TimeSync& timeSync);

  // #region METHOD_HttpServer_begin
  // PURPOSE: Registers routes so the operator can provision and control the gateway.
  void begin();
  // #endregion METHOD_HttpServer_begin
  void handleClient() { server_.handleClient(); }

 private:
  bool requireAuthentication();
  void sendJsonError(int code, const String& error);
  bool readWifiCredentials(RuntimeConfig& candidate, String& error);
  bool readCandidateConfig(RuntimeConfig& candidate, String& error);
  bool checkCommonSendBusy(String& error, int& code);
  int mapModemSendErrorToHttpCode(const String& error) const;
  bool ensureModemReadyForSend(String& error, int& code) const;
  bool handleModemViaSend(const String& to, const String& text);
  bool handleZteViaSend(const String& to, const String& text);
  void handleStatusRequest();
  void handleScanRequest();
  void handleSetupSubmission();
  void handleNetworkSubmission();
  void handleNtpConfigRequest();
  void handleNtpSaveSubmission();
  void handlePasswordSubmission();
  void handleSmtpConfigRequest();
  void handleSmtpSaveSubmission();
  void handleSmtpTestStart();
  void handleSmtpTestStatus();
  void handleZteConfigRequest();
  void handleZteSaveSubmission();
  void handleZteTestStart();
  void handleZteTestStatus();
  void handleZteSendStatus();
  void handleModemStatusRequest();
  void handleModemSourceRequest();
  void handleModemSourceSave();
  void handleModemSendStatus();
  void handleSmsSendStart();
  void handleGpsConfigRequest();
  void handleGpsSaveSubmission();
  void handleGpsStatusRequest();
  void handleTimeStatusRequest();
  void handleWatchdogStatusRequest();
  void handleWatchdogClearRequest();
  void handleRootRedirect();
  void sendPage(const char* path);
  void handleNotFound();

  WebServer& server_;
  ConfigStore& configStore_;
  RuntimeConfig& config_;
  WifiManager& wifi_;
  SmtpService& smtp_;
  ZteService& zte_;
  ModemService& modem_;
  GpsService& gps_;
  TimeSync& timeSync_;
};
// #endregion CLASS_HttpServer
#endif  // SYSTEM_HTTP_SERVER_H
