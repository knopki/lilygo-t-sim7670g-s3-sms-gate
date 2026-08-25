// #region MODULE_CONTRACT
// PURPOSE: Serves the gzipped browser UI from PROGMEM and serializes the JSON
// API responses consumed by that UI, keeping presentation out of firmware
// control flow.
// SCOPE:
// - Asset lookup and serving, JSON escaping, and status/result JSON.
// - NOT: Wi-Fi lifecycle, HTTP route registration, authentication, scans,
//   and persistence.
// INVARIANTS: Every dynamic string is JSON-escaped before serialization and
// credentials are never serialized.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>
#include <WebServer.h>

struct WebStatus {
  bool setupRequired;
  String mode;
  String ssid;
  bool stationConnected;
  String stationIp;
  String macAddress;
  int rssiDbm;
  String mdnsHostname;
  String lastError;
};

struct WebSmtpConfig {
  bool present;
  String host;
  uint16_t port;
  String security;  // "starttls" or "implicit"
  String username;
  bool passwordSet;
  String fromAddress;
  String recipientAddress;
};

struct WebZteConfig {
  bool present;
  bool enabled;
  String host;
  bool passwordSet;
  String label;                   // Phone number or alias shown in forwarded emails.
  uint16_t pollIntervalSec = 15;  // per-source poll period 5..300 s
  String lastStatus;              // empty until the first poll or test completed
};

// #region STRUCT_WebModemSourceConfig
// PURPOSE: Snapshot for GET /api/modem/source without exposing credentials;
// pollIntervalSec is the per-source SMS poll period (5–300 s).
struct WebModemSourceConfig {
  bool present = false;
  bool enabled = false;
  uint16_t pollIntervalSec = 15;
  String label;       // Phone number or alias shown in forwarded emails.
  String lastStatus;  // empty until the first poll completes
};
// #endregion STRUCT_WebModemSourceConfig

// #region STRUCT_WebModemStatus
// PURPOSE: Snapshot for GET /api/modem/status without exposing credentials;
// RSSI/RSRP already converted to dBm, unknown → 0.
struct WebModemStatus {
  bool present = false;
  String cpin;
  int rssiDbm = 0;
  int ber = 99;
  int rsrpDbm = 0;
  int rsrqDb = 0;
  int cereg = -1;
  int creg = -1;
  bool attached = false;
  String oper;
  int act = -1;
  String clock;
  uint16_t smsUsedMe = 0;
  uint16_t smsTotalMe = 0;
  uint16_t smsUsedSm = 0;
  uint16_t smsTotalSm = 0;
  String imei;
  String fw;
};
// #endregion STRUCT_WebModemStatus

// Shape shared by every one-shot asynchronous test route (SMTP, ZTE).
struct WebAsyncOp {
  bool running;
  bool done;
  String result;  // empty until finished
  String message;
};

String escapeJson(const String& value);
void appendJsonString(String& out, const String& value);
String renderStatusJson(const WebStatus& status);
String renderSmtpConfigJson(const WebSmtpConfig& config);
String renderZteConfigJson(const WebZteConfig& config);
// #region FUNC_renderModemStatusJson
// PURPOSE: Serializes WebModemStatus into the /api/modem/status envelope.
String renderModemStatusJson(const WebModemStatus& status);
// #endregion FUNC_renderModemStatusJson
// #region FUNC_renderModemSourceJson
// PURPOSE: Serializes WebModemSourceConfig into the /api/modem/source envelope.
String renderModemSourceJson(const WebModemSourceConfig& config);
// #endregion FUNC_renderModemSourceJson
String renderAsyncOpJson(const WebAsyncOp& op);
String renderMessageJson(const String& message);
String renderErrorJson(const String& error);
void sendJson(WebServer& server, int code, const String& json);
void sendAsset(WebServer& server, const String& path);
