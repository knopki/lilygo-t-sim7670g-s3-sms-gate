// #region MODULE_CONTRACT
// PURPOSE: Keeps network recovery and identity stable across provisioning.
// SCOPE:
// - Manages station, access-point, captive-DNS, and mDNS lifecycle from runtime configuration.
// - NOT: Persisting network credentials or serving HTTP routes.
// INVARIANTS:
// - Device-derived AP and mDNS identities remain stable for the station MAC.
// - A failed station connection retains a provisioning or recovery access path.
// #endregion MODULE_CONTRACT

#include "system/wifi_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <time.h>

#include "system/time_sync.h"
#include "system/watchdog.h"

namespace {
TimeSync* gTimeSyncForSntp = nullptr;
// cppcheck-suppress constParameterCallback
void onSntpSync(struct timeval* tv) {
  if (gTimeSyncForSntp != nullptr && tv != nullptr) {
    int64_t epochMs = (int64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000;
    gTimeSyncForSntp->feedSntpSync(epochMs);
  }
}
}  // namespace

// #region METHOD_WifiManager_buildStationMacAddress
// PURPOSE: Establishes device identity before Wi-Fi starts without relying on interface state.
String WifiManager::buildStationMacAddress() const {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    return F("UNKNOWN");
  }
  char address[18] = {};
  snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return String(address);
}
// #endregion METHOD_WifiManager_buildStationMacAddress

// #region METHOD_WifiManager_buildDeviceSuffix
// PURPOSE: Produces stable identifiers for AP and mDNS recovery paths.
String WifiManager::buildDeviceSuffix() const {
  String compactMac = stationMacAddress_;
  compactMac.replace(":", "");
  return compactMac.length() == 12 ? compactMac.substring(6) : String(F("UNKNOWN"));
}
// #endregion METHOD_WifiManager_buildDeviceSuffix

// #region METHOD_WifiManager_initIdentity
// PURPOSE: Establishes identity before any network interface is started.
void WifiManager::initIdentity() {
  stationMacAddress_ = buildStationMacAddress();
  const String suffix = buildDeviceSuffix();
  accessPointSsid_ = String(F("SMS-Gate-")) + suffix;
  mdnsHostname_ = String(F("sms-gate-")) + suffix;
}
// #endregion METHOD_WifiManager_initIdentity

// #region METHOD_WifiManager_startMdns
// PURPOSE: Makes the online device discoverable by its stable name.
void WifiManager::startMdns() {
  if (mdnsActive_) {
    MDNS.end();
    mdnsActive_ = false;
  }
  if (MDNS.begin(mdnsHostname_.c_str())) {
    MDNS.addService("http", "tcp", kHttpPort);
    mdnsActive_ = true;
    Serial.printf("event=mdns_started hostname=%s.local\n", mdnsHostname_.c_str());
  } else {
    lastConnectionError_ = F("Wi-Fi connected, but mDNS could not start.");
    Serial.println("event=mdns_failed");
  }
}
// #endregion METHOD_WifiManager_startMdns

// #region METHOD_WifiManager_startAccessPoint
// PURPOSE: Provides provisioning or recovery access when STA is unavailable.
void WifiManager::startAccessPoint(const RuntimeConfig& config) {
  const bool initialSetup = config.ssid.length() == 0;
  const wifi_mode_t mode = initialSetup ? WIFI_AP : WIFI_AP_STA;
  Serial.printf("event=ap_start mode=%s ssid=%s\n", initialSetup ? "AP" : "AP+STA",
                accessPointSsid_.c_str());
  if (!WiFi.mode(mode)) {
    lastConnectionError_ = F("Could not select Wi-Fi AP mode.");
    Serial.println("event=ap_failed reason=mode_selection");
    return;
  }

  const bool started = initialSetup
                           ? WiFi.softAP(accessPointSsid_.c_str())
                           : WiFi.softAP(accessPointSsid_.c_str(), config.adminPassword.c_str());
  if (!started) {
    lastConnectionError_ = F("The configuration access point could not start.");
    Serial.println("event=ap_failed reason=softap");
    return;
  }

  dnsServer_.stop();
  dnsServer_.start(kDnsPort, "*", WiFi.softAPIP());
  accessPointActive_ = true;
  Serial.printf("event=ap_active ssid=%s ip=%s\n", accessPointSsid_.c_str(),
                WiFi.softAPIP().toString().c_str());
}
// #endregion METHOD_WifiManager_startAccessPoint

// #region METHOD_WifiManager_stopAccessPoint
// PURPOSE: Releases AP resources after station service is established.
void WifiManager::stopAccessPoint() {
  if (!accessPointActive_) {
    return;
  }
  dnsServer_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  accessPointActive_ = false;
  accessPointShutdownAt_ = 0;
  Serial.println("event=ap_stopped");
}
// #endregion METHOD_WifiManager_stopAccessPoint

// #region METHOD_WifiManager_beginStationAttempt
// PURPOSE: Tests saved access without blocking provisioning or recovery services.
void WifiManager::beginStationAttempt(const RuntimeConfig& config) {
  if (config.ssid.length() == 0) {
    return;
  }
  WiFi.mode(accessPointActive_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
  connectionState_ = ConnectionState::kConnecting;
  connectionAttemptStartedAt_ = millis();
  Serial.printf("event=sta_connect_begin fallback_ap=%s\n", accessPointActive_ ? "true" : "false");
}
// #endregion METHOD_WifiManager_beginStationAttempt

// #region METHOD_WifiManager_startWallClock
// PURPOSE: Starts the configured fallback clock after station connection.
void WifiManager::startWallClock(const RuntimeConfig& config) {
  if (timeSync_ != nullptr) {
    gTimeSyncForSntp = timeSync_;
    esp_sntp_set_time_sync_notification_cb(onSntpSync);
    if (config.ntpEnabled && config.ntpServer1.length() > 0) {
      timeSync_->startSntp(config.ntpServer1.c_str(),
                           config.ntpServer2.length() > 0 ? config.ntpServer2.c_str() : nullptr);
    } else {
      timeSync_->stopSntp();
    }
    return;
  }
  // Fallback when TimeSync not wired (tests/host): direct configTime.
  const char* s1 = config.ntpServer1.length() > 0 ? config.ntpServer1.c_str() : "pool.ntp.org";
  const char* s2 = config.ntpServer2.length() > 0 ? config.ntpServer2.c_str() : "time.nist.gov";
  configTime(0, 0, s1, s2);
  Serial.printf("event=sntp_begin server=%s\n", s1);
}
// #endregion METHOD_WifiManager_startWallClock

// #region METHOD_WifiManager_onStationConnected
// PURPOSE: Completes the online transition and dependent clock setup.
void WifiManager::onStationConnected(const RuntimeConfig& config, bool deferAccessPointShutdown) {
  connectionState_ = ConnectionState::kOnline;
  nextReconnectAt_ = 0;
  lastConnectionError_ = "";
  startWallClock(config);
  startMdns();
  Serial.printf("event=sta_connected ip=%s\n", WiFi.localIP().toString().c_str());
  if (accessPointActive_) {
    if (deferAccessPointShutdown) {
      accessPointShutdownAt_ = millis() + kApShutdownDelayMs;
    } else {
      stopAccessPoint();
    }
  }
}
// #endregion METHOD_WifiManager_onStationConnected

// #region METHOD_WifiManager_enterFallback
// PURPOSE: Preserves recovery access and schedules a later STA retry for any lost saved network.
void WifiManager::enterFallback(const RuntimeConfig& config, const __FlashStringHelper* error,
                                const char* eventName) {
  lastConnectionError_ = error;
  if (!accessPointActive_) {
    startAccessPoint(config);
  }
  connectionState_ = ConnectionState::kFallbackAp;
  nextReconnectAt_ = millis() + kReconnectIntervalMs;
  Serial.printf("event=%s action=fallback_ap\n", eventName);
}
// #endregion METHOD_WifiManager_enterFallback

// #region METHOD_WifiManager_onStationFailed
// PURPOSE: Records failure and enters the protected reconnect path.
void WifiManager::onStationFailed(const RuntimeConfig& config) {
  enterFallback(config, F("Could not connect to the saved Wi-Fi network."), "sta_connect_failed");
}
// #endregion METHOD_WifiManager_onStationFailed

// #region METHOD_WifiManager_buildStatus
// PURPOSE: Produces a secret-free network snapshot for the API.
WebStatus WifiManager::buildStatus(const RuntimeConfig& config) const {
  WebStatus status;
  status.setupRequired = config.ssid.length() == 0;
  switch (connectionState_) {
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
  status.macAddress = stationMacAddress_;
  status.rssiDbm = status.stationConnected ? WiFi.RSSI() : 0;
  status.mdnsHostname = mdnsHostname_;
  status.lastError = lastConnectionError_;
  return status;
}
// #endregion METHOD_WifiManager_buildStatus

// #region METHOD_WifiManager_buildScanNetworksJson
// PURPOSE: Gives provisioning safe, bounded choices from the current scan.
String WifiManager::buildScanNetworksJson() {
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
// #endregion METHOD_WifiManager_buildScanNetworksJson

// #region METHOD_WifiManager_testStationCandidate
// PURPOSE: Verifies replacement credentials before they are saved.
bool WifiManager::testStationCandidate(const RuntimeConfig& candidate,
                                       const RuntimeConfig& previous) {
  Serial.println("event=sta_candidate_test_begin");
  WiFi.mode(accessPointActive_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect();
  delay(kWiFiTestStepMs);
  WiFi.begin(candidate.ssid.c_str(), candidate.wifiPassword.c_str());
  const unsigned long deadline = millis() + kConnectTimeoutMs;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("event=sta_candidate_test_complete connected=true");
      return true;
    }
    watchdog::feedLoop();
    delay(kWiFiTestStepMs);
  }

  Serial.println("event=sta_candidate_test_complete connected=false");
  if (previous.ssid.length() > 0) {
    Serial.println("event=sta_previous_profile_restore_begin");
    WiFi.disconnect();
    WiFi.begin(previous.ssid.c_str(), previous.wifiPassword.c_str());
    connectionState_ = ConnectionState::kConnecting;
    connectionAttemptStartedAt_ = millis();
  }
  return false;
}
// #endregion METHOD_WifiManager_testStationCandidate

// #region METHOD_WifiManager_loop
// PURPOSE: Advances STA/AP recovery without blocking other services.
void WifiManager::loop(const RuntimeConfig& config) {
  handleDns();
  const unsigned long now = millis();
  if (connectionState_ == ConnectionState::kConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      onStationConnected(config, false);
    } else if (now - connectionAttemptStartedAt_ >= kConnectTimeoutMs) {
      onStationFailed(config);
    }
  } else if (connectionState_ == ConnectionState::kOnline && WiFi.status() != WL_CONNECTED) {
    enterFallback(config, F("Connection to the saved Wi-Fi network was lost."), "sta_disconnected");
  } else if (connectionState_ == ConnectionState::kFallbackAp && config.ssid.length() > 0 &&
             now >= nextReconnectAt_) {
    beginStationAttempt(config);
  }

  if (accessPointShutdownAt_ > 0 && now >= accessPointShutdownAt_ &&
      WiFi.status() == WL_CONNECTED) {
    stopAccessPoint();
  }
  if (accessPointRestartAt_ > 0 && now >= accessPointRestartAt_ && accessPointActive_) {
    accessPointRestartAt_ = 0;
    dnsServer_.stop();
    WiFi.softAPdisconnect(true);
    accessPointActive_ = false;
    startAccessPoint(config);
  }
}
// #endregion METHOD_WifiManager_loop

// #region METHOD_WifiManager_handleDns
// PURPOSE: Keeps captive-portal resolution available while AP is active.
void WifiManager::handleDns() {
  if (accessPointActive_) {
    dnsServer_.processNextRequest();
  }
}
// #endregion METHOD_WifiManager_handleDns

// #region METHOD_WifiManager_scheduleAccessPointRestart
// PURPOSE: Defers AP restart until active HTTP work can finish.
void WifiManager::scheduleAccessPointRestart() {
  if (accessPointActive_) {
    accessPointRestartAt_ = millis() + kApShutdownDelayMs;
  }
}
// #endregion METHOD_WifiManager_scheduleAccessPointRestart
