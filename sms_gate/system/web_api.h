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
#ifndef SYSTEM_WEB_API_H
#define SYSTEM_WEB_API_H

#include <Arduino.h>
#include <WebServer.h>

// #region STRUCT_WebStatus
// PURPOSE: Snapshot for GET /api/status; stationIp null when disconnected.
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
// PURPOSE: Snapshot for GET /api/ntp; servers already trimmed, defaults
// applied when enabled and empty.
struct WebNtpConfig {
  bool ntpEnabled = true;
  String ntpServer1;
  String ntpServer2;
};
// #endregion STRUCT_WebNtpConfig

// #region STRUCT_WebSmtpConfig
// PURPOSE: Snapshot for GET /api/smtp without password; security is starttls/implicit.
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
// PURPOSE: Snapshot for GET /api/zte without password; pollIntervalSec 5..300 s.
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
// PURPOSE: Snapshot for GET /api/modem/source without exposing credentials;
// pollIntervalSec is the per-source SMS poll period (5–300 s).
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
// PURPOSE: Common poll/label/status slice shared by ZTE and modem-source JSON.
struct WebSourceConfigCommon {
  bool present = false;
  uint16_t pollIntervalSec = 15;
  String label;
  String lastStatus;
};
// #endregion STRUCT_WebSourceConfigCommon

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

// #region STRUCT_WebGpsConfig
// PURPOSE: Snapshot for GET /api/gps without exposing credentials;
// pollIntervalSec 5..300 s, default 60 s.
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
// PURPOSE: Snapshot for GET /api/time — TimeSync arbitration result.
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
// PURPOSE: Snapshot for GET /api/watchdog — WDT timeout, boot-loop count,
// safe-mode and last reset reason.
struct WebWatchdogStatus {
  bool safeMode = false;
  uint32_t bootCount = 0;
  uint32_t timeoutSec = 60;
  uint32_t lastResetReason = 0;
  uint32_t uptimeMs = 0;
};
// #endregion STRUCT_WebWatchdogStatus

// #region STRUCT_WebGpsStatus
// PURPOSE: Snapshot for GET /api/gps/status — fix, coords, sats and UTC.
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
// PURPOSE: Snapshot for one-shot async routes (SMTP/ZTE/modem test/send).
struct WebAsyncOp {
  bool running;
  bool done;
  String result;  // empty until finished
  String message;
};
// #endregion STRUCT_WebAsyncOp

String escapeJson(const String& value);
void appendJsonString(String& out, const String& value);
String renderStatusJson(const WebStatus& status);
String renderNtpConfigJson(const WebNtpConfig& config);
String renderSmtpConfigJson(const WebSmtpConfig& config);
String renderZteConfigJson(const WebZteConfig& config);
// #region FUNC_renderModemStatusJson
// PURPOSE: Serializes WebModemStatus into the /api/modem/status envelope.
String renderTimeStatusJson(const WebTimeStatus& status);
String renderWatchdogStatusJson(const WebWatchdogStatus& status);
String renderModemStatusJson(const WebModemStatus& status);
// #endregion FUNC_renderModemStatusJson
// #region FUNC_renderModemSourceJson
// PURPOSE: Serializes WebModemSourceConfig into the /api/modem/source envelope.
String renderModemSourceJson(const WebModemSourceConfig& config);
// #endregion FUNC_renderModemSourceJson
String renderGpsConfigJson(const WebGpsConfig& config);
String renderGpsStatusJson(const WebGpsStatus& status);
String renderAsyncOpJson(const WebAsyncOp& op);
String renderMessageJson(const String& message);
String renderErrorJson(const String& error);
void sendJson(WebServer& server, int code, const String& json);
void sendAsset(WebServer& server, const String& path);
#endif  // SYSTEM_WEB_API_H
