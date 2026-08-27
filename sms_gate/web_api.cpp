// #region MODULE_CONTRACT
// PURPOSE: Serves the gzipped browser UI from PROGMEM and serializes the JSON
// API responses consumed by that UI, keeping presentation out of firmware
// control flow.
// INVARIANTS: Every dynamic string is JSON-escaped before serialization and
// credentials are never serialized.
// #endregion MODULE_CONTRACT

#include "system/web_api.h"

#include "web_assets.h"

String escapeJson(const String& value);
void appendJsonString(String& out, const String& value);

namespace {

void appendJsonNullableString(String& out, bool present, const String& value) {
  if (!present) {
    out += F("null");
    return;
  }
  appendJsonString(out, value);
}

void appendPresent(String& out, bool present) {
  out += F("{\"present\":");
  out += present ? F("true") : F("false");
}

void appendSourceCommonFields(String& out, const WebSourceConfigCommon& common,
                              bool labelBeforePoll) {
  if (labelBeforePoll) {
    out += F(",\"label\":");
    appendJsonString(out, common.label);
    out += F(",\"poll_interval\":");
    out += String(common.pollIntervalSec);
  } else {
    out += F(",\"poll_interval\":");
    out += String(common.pollIntervalSec);
    out += F(",\"label\":");
    appendJsonString(out, common.label);
  }
  out += F(",\"last_status\":");
  appendJsonNullableString(out, common.lastStatus.length() > 0, common.lastStatus);
}

}  // namespace

namespace {
constexpr size_t kEscapeJsonReserveExtra = 8;
constexpr size_t kStatusJsonReserve = 320;
constexpr size_t kSmtpJsonReserve = 256;
constexpr size_t kZteJsonReserve = 340;
constexpr size_t kModemSourceReserve = 360;
constexpr size_t kModemStatusReserve = 420;
constexpr size_t kGpsConfigReserve = 240;
constexpr size_t kGpsStatusReserve = 320;
constexpr size_t kAsyncOpReserveExtra = 64;
constexpr size_t kTimeReserve = 160;
constexpr size_t kMessageReserveExtra = 32;
}  // namespace

// #region FUNC_escapeJson
// PURPOSE: Escapes control characters and quotes for safe JSON embedding.
String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + kEscapeJsonReserveExtra);
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
// #endregion FUNC_escapeJson

// #region FUNC_appendJsonString
// PURPOSE: Appends a JSON-quoted string with escaping to the output buffer.
void appendJsonString(String& out, const String& value) {
  out += '"';
  out += escapeJson(value);
  out += '"';
}
// #endregion FUNC_appendJsonString

// #region FUNC_renderStatusJson
// PURPOSE: Serializes WebStatus into the /api/status envelope.
String renderStatusJson(const WebStatus& status) {
  String json;
  json.reserve(kStatusJsonReserve + 128);
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
// #endregion FUNC_renderStatusJson

// #region FUNC_renderNtpConfigJson
// PURPOSE: Serializes WebNtpConfig into the /api/ntp envelope.
String renderNtpConfigJson(const WebNtpConfig& config) {
  String json;
  json.reserve(96 + config.ntpServer1.length() + config.ntpServer2.length());
  json += F("{\"ntp_enabled\":");
  json += config.ntpEnabled ? F("true") : F("false");
  json += F(",\"ntp_server1\":");
  appendJsonString(json, config.ntpServer1);
  json += F(",\"ntp_server2\":");
  appendJsonString(json, config.ntpServer2);
  json += '}';
  return json;
}
// #endregion FUNC_renderNtpConfigJson

// #region FUNC_renderSmtpConfigJson
// PURPOSE: Serializes WebSmtpConfig into the /api/smtp envelope without exposing the password.
String renderSmtpConfigJson(const WebSmtpConfig& config) {
  String json;
  json.reserve(kSmtpJsonReserve);
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
// #endregion FUNC_renderSmtpConfigJson

// #region FUNC_renderZteConfigJson
// PURPOSE: Serializes WebZteConfig into the /api/zte envelope without exposing the password.
String renderZteConfigJson(const WebZteConfig& config) {
  String json;
  json.reserve(kZteJsonReserve);
  appendPresent(json, config.present);
  json += F(",\"module_enabled\":");
  json += config.moduleEnabled ? F("true") : F("false");
  json += F(",\"forward_enabled\":");
  json += config.forwardEnabled ? F("true") : F("false");
  json += F(",\"host\":");
  appendJsonString(json, config.host);
  json += F(",\"password_set\":");
  json += config.passwordSet ? F("true") : F("false");
  WebSourceConfigCommon common{config.present, config.pollIntervalSec, config.label,
                               config.lastStatus};
  appendSourceCommonFields(json, common, true);
  json += '}';
  return json;
}
// #endregion FUNC_renderZteConfigJson

// #region FUNC_renderModemSourceJson
// PURPOSE: Serializes WebModemSourceConfig into the /api/modem/source envelope.
String renderModemSourceJson(const WebModemSourceConfig& config) {
  String json;
  json.reserve(kModemSourceReserve);
  appendPresent(json, config.present);
  json += F(",\"module_enabled\":");
  json += config.moduleEnabled ? F("true") : F("false");
  json += F(",\"poll_enabled\":");
  json += config.pollEnabled ? F("true") : F("false");
  json += F(",\"sms_poll_enabled\":");
  json += config.smsPollEnabled ? F("true") : F("false");
  json += F(",\"nitz_time_sync_enabled\":");
  json += config.nitzTimeSyncEnabled ? F("true") : F("false");
  WebSourceConfigCommon common{config.present, config.pollIntervalSec, config.label,
                               config.lastStatus};
  appendSourceCommonFields(json, common, false);
  json += '}';
  return json;
}
// #endregion FUNC_renderModemSourceJson

// #region FUNC_renderModemStatusJson
// PURPOSE: Serializes WebModemStatus into the /api/modem/status envelope.
String renderModemStatusJson(const WebModemStatus& status) {
  String json;
  json.reserve(kModemStatusReserve);
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

// #region FUNC_renderGpsConfigJson
// PURPOSE: Serializes WebGpsConfig into /api/gps envelope.
String renderGpsConfigJson(const WebGpsConfig& config) {
  String json;
  json.reserve(kGpsConfigReserve);
  appendPresent(json, config.present);
  json += F(",\"module_enabled\":");
  json += config.moduleEnabled ? F("true") : F("false");
  json += F(",\"poll_enabled\":");
  json += config.pollEnabled ? F("true") : F("false");
  json += F(",\"time_sync_enabled\":");
  json += config.timeSyncEnabled ? F("true") : F("false");
  json += F(",\"poll_interval\":");
  json += String(config.pollIntervalSec);
  json += F(",\"last_status\":");
  appendJsonNullableString(json, config.lastStatus.length() > 0, config.lastStatus);
  json += '}';
  return json;
}
// #endregion FUNC_renderGpsConfigJson

// #region FUNC_renderWatchdogStatusJson
String renderWatchdogStatusJson(const WebWatchdogStatus& status) {
  String json;
  json.reserve(180);
  json += F("{\"safe_mode\":");
  json += status.safeMode ? F("true") : F("false");
  json += F(",\"boot_count\":");
  json += String(status.bootCount);
  json += F(",\"timeout_sec\":");
  json += String(status.timeoutSec);
  json += F(",\"last_reset_reason\":");
  json += String(status.lastResetReason);
  json += F(",\"uptime_ms\":");
  json += String(status.uptimeMs);
  json += '}';
  return json;
}
// #endregion FUNC_renderWatchdogStatusJson

// #region FUNC_renderTimeStatusJson
String renderTimeStatusJson(const WebTimeStatus& status) {
  String json;
  json.reserve(kTimeReserve + 32);
  json += F("{\"source\":");
  appendJsonString(json, status.source);
  json += F(",\"stratum\":");
  json += String(status.stratum);
  json += F(",\"dispersion_ms\":");
  json += String(status.dispersionMs);
  json += F(",\"epoch_ms\":");
  json += String((long long)status.epochMs);
  json += F(",\"last_sync_ms\":");
  json += String(status.lastSyncMs);
  json += F(",\"quarantined\":");
  json += status.quarantined ? F("true") : F("false");
  json += F(",\"quarantined_until_ms\":");
  json += String(status.quarantinedUntilMs);
  json += '}';
  return json;
}
// #endregion FUNC_renderTimeStatusJson

// #region FUNC_renderGpsStatusJson
// PURPOSE: Serializes WebGpsStatus into /api/gps/status envelope.
String renderGpsStatusJson(const WebGpsStatus& status) {
  String json;
  json.reserve(kGpsStatusReserve);
  json += F("{\"present\":");
  json += status.present ? F("true") : F("false");
  json += F(",\"powered\":");
  json += status.powered ? F("true") : F("false");
  json += F(",\"fix\":");
  json += status.fix ? F("true") : F("false");
  json += F(",\"mode\":");
  json += String(status.mode);
  json += F(",\"sats\":{\"used\":");
  json += String(status.satsUsed);
  json += F(",\"visible\":");
  json += String(status.satsVisible);
  json += F("}");
  json += F(",\"coords\":{\"lat\":");
  json += String(status.lat, 6);
  json += F(",\"lon\":");
  json += String(status.lon, 6);
  json += F(",\"alt\":");
  json += String(status.alt, 1);
  json += F("}");
  json += F(",\"speed\":");
  json += String(status.speed, 1);
  json += F(",\"course\":");
  json += String(status.course, 1);
  json += F(",\"time\":{\"date\":");
  appendJsonNullableString(json, status.date.length() > 0, status.date);
  json += F(",\"utc\":");
  appendJsonNullableString(json, status.utcTime.length() > 0, status.utcTime);
  json += F(",\"iso\":");
  appendJsonNullableString(json, status.isoTime.length() > 0, status.isoTime);
  json += F("}");
  json += F(",\"updated_ms\":");
  json += String(status.updatedMs);
  json += '}';
  return json;
}
// #endregion FUNC_renderGpsStatusJson

// #region FUNC_renderAsyncOpJson
// PURPOSE: Serializes WebAsyncOp (running/done/result/message) for polling test/send routes.
String renderAsyncOpJson(const WebAsyncOp& op) {
  String json;
  json.reserve(op.message.length() + kAsyncOpReserveExtra);
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
// #endregion FUNC_renderAsyncOpJson

// #region FUNC_renderMessageJson
// PURPOSE: Serializes a success envelope {ok:true,message}.
String renderMessageJson(const String& message) {
  String json;
  json.reserve(message.length() + kMessageReserveExtra);
  json += F("{\"ok\":true,\"message\":");
  appendJsonString(json, message);
  json += '}';
  return json;
}
// #endregion FUNC_renderMessageJson

// #region FUNC_renderErrorJson
// PURPOSE: Serializes an error envelope {ok:false,error}.
String renderErrorJson(const String& error) {
  String json;
  json.reserve(error.length() + kMessageReserveExtra);
  json += F("{\"ok\":false,\"error\":");
  appendJsonString(json, error);
  json += '}';
  return json;
}
// #endregion FUNC_renderErrorJson

// #region FUNC_sendJson
// PURPOSE: Sends a JSON response with the correct content type.
void sendJson(WebServer& server, int code, const String& json) {
  server.send(code, "application/json; charset=utf-8", json);
}
// #endregion FUNC_sendJson

// #region FUNC_sendAsset
// PURPOSE: Serves a gzipped asset from PROGMEM with long-term caching headers; falls back to 404.
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
// #endregion FUNC_sendAsset
