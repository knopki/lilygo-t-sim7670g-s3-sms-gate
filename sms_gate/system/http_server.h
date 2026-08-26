// #region MODULE_CONTRACT
// PURPOSE: Owns HTTP Digest authentication, the route table and
// the SendGate (single-flight) guards so sms_gate.ino only drives
// setup/loop and lifecycle delegation.
// SCOPE:
// - 24 HTTP routes: /, /app.js, /style.css, GET /api/status, GET
//   /api/scan, POST /api/setup, POST /api/network, POST /api/password,
//   GET/POST /api/smtp, POST/GET /api/smtp/test, GET/POST /api/zte,
//   POST/GET /api/zte/test, POST/GET /api/zte/send, POST/GET
//   /api/modem/send, POST /api/sms/send (via=zte|modem), GET
//   /api/modem/status, GET/POST /api/modem/source, and the 302/404
//   captive-portal NotFound handler; Digest authentication against the
//   stored admin password (realm "SMS Gate", user "admin"); JSON error
//   envelope, candidate Wi-Fi validation, and the common SendGate helper
//   that serialises outgoing SMS against poll/test/send busy states.
// - NOT: Wi-Fi STA/AP state machine, NVS persistence, SMTP/TLS, ZTE
//   goform, modem AT, asset gzip generation, and email rendering.
// INVARIANTS: Credentials are never written to Serial or returned in
// HTTP responses; the SMTP/modem/ZTE passwords are never serialized;
// at most one test/send/poll owns the external modem at a time; every
// validation error is a JSON envelope with an operator message; 409 is
// used for SendGate busy, 503 for modem unreachable, 400 for form
// validation.
// DEPENDENCIES: Uses Arduino-ESP32 WebServer and WiFi; delegates
// persistence to ConfigStore, status/scan/validation to WifiManager,
// SMTP delivery to SmtpService, ZTE source to ZteService, modem source
// to ModemService, and JSON/assets to web_api.h.
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
  void begin();
  void handleClient() { server_.handleClient(); }

 private:
  bool requireAuthentication();
  void sendJsonError(int code, const String& error);
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
  void handlePasswordSubmission();
  void handleSmtpConfigRequest();
  void handleSmtpSaveSubmission();
  void handleSmtpTestStart();
  void handleSmtpTestStatus();
  void handleZteConfigRequest();
  void handleZteSaveSubmission();
  void handleZteTestStart();
  void handleZteTestStatus();
  void handleZteSendStart();
  void handleZteSendStatus();
  void handleModemStatusRequest();
  void handleModemSourceRequest();
  void handleModemSourceSave();
  void handleModemSendStart();
  void handleModemSendStatus();
  void handleSmsSendStart();
  void handleGpsConfigRequest();
  void handleGpsSaveSubmission();
  void handleGpsStatusRequest();
  void handleTimeStatusRequest();
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
