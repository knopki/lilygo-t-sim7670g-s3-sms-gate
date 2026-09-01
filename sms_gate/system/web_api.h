// #region MODULE_CONTRACT
// PURPOSE: Keeps browser presentation and JSON safety out of control flow.
// SCOPE:
// - Asset lookup and serving, JSON escaping, and status/result JSON.
// - NOT: Wi-Fi lifecycle, HTTP route registration, authentication, scans,
//   and persistence.
// INVARIANTS:
// - Every dynamic string is JSON-escaped before serialization and
//   credentials are never serialized.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_WEB_API_H
#define SYSTEM_WEB_API_H

#include <Arduino.h>
#include <WebServer.h>

// #region STRUCT_WebStatus
// PURPOSE: Carries a secret-free network snapshot so the UI can distinguish recovery states.
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
// #endregion STRUCT_WebStatus

// #region STRUCT_WebNtpConfig
// PURPOSE: Carries normalized clock settings so setup and settings share one API shape.
struct WebNtpConfig {
  bool ntpEnabled = true;
  String ntpServer1;
  String ntpServer2;
};
// #endregion STRUCT_WebNtpConfig

// #region STRUCT_WebSmtpConfig
// PURPOSE: Carries editable SMTP state without returning the stored password to the browser.
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
// #endregion STRUCT_WebSmtpConfig

// #region STRUCT_WebZteConfig
// PURPOSE: Carries editable ZTE state while keeping modem credentials out of the browser.
struct WebZteConfig {
  bool present;
  bool moduleEnabled = false;
  bool forwardEnabled = false;
  String host;
  bool passwordSet = false;
  String label;                   // Phone number or alias shown in forwarded emails.
  uint16_t pollIntervalSec = 15;  // per-source poll period 5..300 s
  String lastStatus;              // empty until the first poll or test completed
};
// #endregion STRUCT_WebZteConfig

// #region STRUCT_WebModemSourceConfig
// PURPOSE: Carries editable modem-source state without exposing credentials to the browser.
struct WebModemSourceConfig {
  bool present = false;
  bool moduleEnabled = false;
  bool pollEnabled = false;
  bool smsPollEnabled = false;
  bool nitzTimeSyncEnabled = false;
  uint16_t pollIntervalSec = 15;
  String label;       // Phone number or alias shown in forwarded emails.
  String lastStatus;  // empty until the first poll completes
};
// #endregion STRUCT_WebModemSourceConfig

// #region STRUCT_WebSourceConfigCommon
// PURPOSE: Keeps source pages aligned on shared poll, alias, and outcome fields.
struct WebSourceConfigCommon {
  bool present = false;
  uint16_t pollIntervalSec = 15;
  String label;
  String lastStatus;
};
// #endregion STRUCT_WebSourceConfigCommon

// #region STRUCT_WebModemStatus
// PURPOSE: Carries modem diagnostics without exposing credentials or raw transport state.
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

// #region STRUCT_WebGpsConfig
// PURPOSE: Carries GNSS controls and outcome state without exposing modem internals.
struct WebGpsConfig {
  bool present = false;
  bool moduleEnabled = false;
  bool pollEnabled = false;
  bool timeSyncEnabled = false;
  uint16_t pollIntervalSec = 60;
  String lastStatus;
};
// #endregion STRUCT_WebGpsConfig

// #region STRUCT_WebTimeStatus
// PURPOSE: Carries clock quality so clients can tell whether displayed time is trustworthy.
struct WebTimeStatus {
  String source;  // unsynced/sntp/nitz/gnss
  uint8_t stratum = 0;
  uint32_t dispersionMs = 0;
  int64_t epochMs = 0;
  int64_t lastSyncEpochMs = 0;
  bool quarantined = false;
  int64_t quarantinedUntilEpochMs = 0;
};
// #endregion STRUCT_WebTimeStatus

// #region STRUCT_WebWatchdogStatus
// PURPOSE: Carries recovery state so operators can diagnose boot loops without hardware access.
struct WebWatchdogStatus {
  bool safeMode = false;
  uint32_t bootCount = 0;
  uint32_t timeoutSec = 60;
  uint32_t lastResetReason = 0;
  uint32_t uptimeMs = 0;
};
// #endregion STRUCT_WebWatchdogStatus

// #region STRUCT_WebGpsStatus
// PURPOSE: Carries one GNSS snapshot so operators can distinguish fix and time state.
struct WebGpsStatus {
  bool present = false;
  bool powered = false;
  bool fix = false;
  int mode = -1;
  int satsUsed = 0;
  int satsVisible = 0;
  int satsGps = 0;
  int satsGlonass = 0;
  int satsGalileo = 0;
  int satsBeidou = 0;
  double lat = 0.0;
  double lon = 0.0;
  float alt = 0.0f;
  float speed = 0.0f;
  float course = 0.0f;
  String date;
  String utcTime;
  String isoTime;
  uint32_t updatedMs = 0;
};
// #endregion STRUCT_WebGpsStatus

// #region STRUCT_WebAsyncOp
// PURPOSE: Carries non-blocking operation state so one-shot routes can be polled uniformly.
struct WebAsyncOp {
  bool running;
  bool done;
  String result;  // empty until finished
  String message;
};
// #endregion STRUCT_WebAsyncOp

// #region FUNC_escapeJson
// PURPOSE: Prevents dynamic text from breaking JSON responses.
String escapeJson(const String& value);
// #endregion FUNC_escapeJson

// #region FUNC_appendJsonString
// PURPOSE: Prevents untrusted text from changing API field structure.
void appendJsonString(String& out, const String& value);
// #endregion FUNC_appendJsonString

// #region FUNC_renderStatusJson
// PURPOSE: Gives the UI a stable network-status response.
String renderStatusJson(const WebStatus& status);
// #endregion FUNC_renderStatusJson

// #region FUNC_renderNtpConfigJson
// PURPOSE: Gives the UI a safe view of clock-source configuration.
String renderNtpConfigJson(const WebNtpConfig& config);
// #endregion FUNC_renderNtpConfigJson

// #region FUNC_renderSmtpConfigJson
// PURPOSE: Lets the UI manage SMTP without exposing credentials.
String renderSmtpConfigJson(const WebSmtpConfig& config);
// #endregion FUNC_renderSmtpConfigJson

// #region FUNC_renderZteConfigJson
// PURPOSE: Lets the UI manage ZTE without exposing credentials.
String renderZteConfigJson(const WebZteConfig& config);
// #endregion FUNC_renderZteConfigJson
// #region FUNC_renderTimeStatusJson
// PURPOSE: Lets clients observe clock quality and source arbitration.
String renderTimeStatusJson(const WebTimeStatus& status);
// #endregion FUNC_renderTimeStatusJson

// #region FUNC_renderWatchdogStatusJson
// PURPOSE: Lets operators see recovery state without hardware access.
String renderWatchdogStatusJson(const WebWatchdogStatus& status);
// #endregion FUNC_renderWatchdogStatusJson

// #region FUNC_renderModemStatusJson
// PURPOSE: Lets the UI diagnose modem state without credentials.
String renderModemStatusJson(const WebModemStatus& status);
// #endregion FUNC_renderModemStatusJson

// #region FUNC_renderModemSourceJson
// PURPOSE: Lets the UI manage the modem source without credentials.
String renderModemSourceJson(const WebModemSourceConfig& config);
// #endregion FUNC_renderModemSourceJson
// #region FUNC_renderGpsConfigJson
// PURPOSE: Lets the UI manage GNSS without exposing device internals.
String renderGpsConfigJson(const WebGpsConfig& config);
// #endregion FUNC_renderGpsConfigJson

// #region FUNC_renderGpsStatusJson
// PURPOSE: Lets operators diagnose GNSS state from one snapshot.
String renderGpsStatusJson(const WebGpsStatus& status);
// #endregion FUNC_renderGpsStatusJson

// #region FUNC_renderAsyncOpJson
// PURPOSE: Lets clients follow one-shot operations without blocking.
String renderAsyncOpJson(const WebAsyncOp& op);
// #endregion FUNC_renderAsyncOpJson

// #region FUNC_renderMessageJson
// PURPOSE: Keeps successful route responses uniform for clients.
String renderMessageJson(const String& message);
// #endregion FUNC_renderMessageJson

// #region FUNC_renderErrorJson
// PURPOSE: Keeps validation and operation failures uniform for clients.
String renderErrorJson(const String& error);
// #endregion FUNC_renderErrorJson

// #region FUNC_sendJson
// PURPOSE: Ensures routes return the API's declared JSON contract.
void sendJson(WebServer& server, int code, const String& json);
// #endregion FUNC_sendJson

// #region FUNC_sendAsset
// PURPOSE: Lets the UI load assets without filesystem access.
void sendAsset(WebServer& server, const String& path);
// #endregion FUNC_sendAsset
#endif  // SYSTEM_WEB_API_H
