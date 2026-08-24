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
  String label;       // Phone number or alias shown in forwarded emails.
  String lastStatus;  // empty until the first poll or test completed
};

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
String renderAsyncOpJson(const WebAsyncOp& op);
String renderMessageJson(const String& message);
String renderErrorJson(const String& error);
void sendJson(WebServer& server, int code, const String& json);
void sendAsset(WebServer& server, const String& path);
