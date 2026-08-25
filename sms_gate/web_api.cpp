// #region MODULE_CONTRACT
// PURPOSE: Serves the gzipped browser UI from PROGMEM and serializes the JSON
// API responses consumed by that UI, keeping presentation out of firmware
// control flow.
// INVARIANTS: Every dynamic string is JSON-escaped before serialization and
// credentials are never serialized.
// #endregion MODULE_CONTRACT

#include "web_api.h"

#include "web_assets.h"

namespace {

void appendJsonNullableString(String& out, bool present, const String& value) {
  if (!present) {
    out += F("null");
    return;
  }
  appendJsonString(out, value);
}

}  // namespace

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  char buffer[8];
  for (size_t index = 0; index < value.length(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    switch (ch) {
      case '"':
        escaped += F("\\\"");
        break;
      case '\\':
        escaped += F("\\\\");
        break;
      case '\b':
        escaped += F("\\b");
        break;
      case '\f':
        escaped += F("\\f");
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        if (ch < 0x20) {
          snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          escaped += buffer;
        } else {
          escaped += static_cast<char>(ch);
        }
        break;
    }
  }
  return escaped;
}

void appendJsonString(String& out, const String& value) {
  out += '"';
  out += escapeJson(value);
  out += '"';
}

String renderStatusJson(const WebStatus& status) {
  String json;
  json.reserve(320);
  json += F("{\"setup_required\":");
  json += status.setupRequired ? F("true") : F("false");
  json += F(",\"mode\":");
  appendJsonString(json, status.mode);
  json += F(",\"ssid\":");
  appendJsonString(json, status.ssid);
  json += F(",\"station_ip\":");
  appendJsonNullableString(json, status.stationConnected, status.stationIp);
  json += F(",\"mac\":");
  appendJsonString(json, status.macAddress);
  json += F(",\"rssi_dbm\":");
  json += status.stationConnected ? String(status.rssiDbm) : String(F("null"));
  json += F(",\"mdns_hostname\":");
  appendJsonString(json, status.mdnsHostname);
  json += F(",\"last_error\":");
  appendJsonNullableString(json, status.lastError.length() > 0, status.lastError);
  json += '}';
  return json;
}

String renderSmtpConfigJson(const WebSmtpConfig& config) {
  String json;
  json.reserve(256);
  json += F("{\"present\":");
  json += config.present ? F("true") : F("false");
  json += F(",\"host\":");
  appendJsonString(json, config.host);
  json += F(",\"port\":");
  json += String(config.port);
  json += F(",\"security\":");
  appendJsonString(json, config.security);
  json += F(",\"username\":");
  appendJsonString(json, config.username);
  json += F(",\"password_set\":");
  json += config.passwordSet ? F("true") : F("false");
  json += F(",\"from\":");
  appendJsonString(json, config.fromAddress);
  json += F(",\"recipient\":");
  appendJsonString(json, config.recipientAddress);
  json += '}';
  return json;
}

String renderZteConfigJson(const WebZteConfig& config) {
  String json;
  json.reserve(256);
  json += F("{\"present\":");
  json += config.present ? F("true") : F("false");
  json += F(",\"enabled\":");
  json += config.enabled ? F("true") : F("false");
  json += F(",\"host\":");
  appendJsonString(json, config.host);
  json += F(",\"password_set\":");
  json += config.passwordSet ? F("true") : F("false");
  json += F(",\"label\":");
  appendJsonString(json, config.label);
  json += F(",\"last_status\":");
  appendJsonNullableString(json, config.lastStatus.length() > 0, config.lastStatus);
  json += '}';
  return json;
}

// #region FUNC_renderModemStatusJson
// PURPOSE: Serializes WebModemStatus into the /api/modem/status envelope.
String renderModemStatusJson(const WebModemStatus& status) {
  String json;
  json.reserve(420);
  json += F("{\"present\":");
  json += status.present ? F("true") : F("false");
  json += F(",\"cpin\":");
  appendJsonString(json, status.cpin);
  json += F(",\"signal\":{\"rssi_dbm\":");
  json += String(status.rssiDbm);
  json += F(",\"ber\":");
  json += String(status.ber);
  json += F(",\"rsrp_dbm\":");
  json += String(status.rsrpDbm);
  json += F(",\"rsrq_db\":");
  json += String(status.rsrqDb);
  json += F("}");
  json += F(",\"registration\":{\"cereg\":");
  json += String(status.cereg);
  json += F(",\"creg\":");
  json += String(status.creg);
  json += F(",\"attached\":");
  json += status.attached ? F("true") : F("false");
  json += F("}");
  json += F(",\"operator\":{\"name\":");
  appendJsonString(json, status.oper);
  json += F(",\"act\":");
  json += String(status.act);
  json += F("}");
  json += F(",\"clock\":");
  appendJsonNullableString(json, status.clock.length() > 0, status.clock);
  json += F(",\"sms_storage\":{\"mem\":\"ME\",\"used\":");
  json += String(status.smsUsedMe);
  json += F(",\"total\":");
  json += String(status.smsTotalMe);
  json += F(",\"mem2\":\"SM\",\"used2\":");
  json += String(status.smsUsedSm);
  json += F(",\"total2\":");
  json += String(status.smsTotalSm);
  json += F("}");
  json += F(",\"identity\":{\"imei\":");
  appendJsonString(json, status.imei);
  json += F(",\"fw\":");
  appendJsonString(json, status.fw);
  json += F("}");
  json += '}';
  return json;
}
// #endregion FUNC_renderModemStatusJson

String renderAsyncOpJson(const WebAsyncOp& op) {
  String json;
  json.reserve(op.message.length() + 64);
  json += F("{\"running\":");
  json += op.running ? F("true") : F("false");
  json += F(",\"done\":");
  json += op.done ? F("true") : F("false");
  json += F(",\"result\":");
  appendJsonNullableString(json, op.result.length() > 0, op.result);
  json += F(",\"message\":");
  appendJsonNullableString(json, op.message.length() > 0, op.message);
  json += '}';
  return json;
}

String renderMessageJson(const String& message) {
  String json;
  json.reserve(message.length() + 32);
  json += F("{\"ok\":true,\"message\":");
  appendJsonString(json, message);
  json += '}';
  return json;
}

String renderErrorJson(const String& error) {
  String json;
  json.reserve(error.length() + 32);
  json += F("{\"ok\":false,\"error\":");
  appendJsonString(json, error);
  json += '}';
  return json;
}

void sendJson(WebServer& server, int code, const String& json) {
  server.send(code, "application/json; charset=utf-8", json);
}

void sendAsset(WebServer& server, const String& path) {
  for (const web_assets::Asset& asset : web_assets::kAssets) {
    if (path != asset.path) {
      continue;
    }
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, asset.contentType, reinterpret_cast<PGM_P>(asset.data), asset.size);
    return;
  }
  server.send(404, "text/plain", "Not found.");
}
