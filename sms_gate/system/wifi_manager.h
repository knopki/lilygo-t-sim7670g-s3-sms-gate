// #region MODULE_CONTRACT
// PURPOSE: Owns the station Wi-Fi lifecycle, the fallback access point with
// captive DNS, mDNS publication and SNTP wall-clock start so the remaining
// firmware only calls begin/loop/status/scan.
// SCOPE:
// - Station MAC and device suffix, AP SSID and mDNS hostname, AP/STA mode
//   selection, connection attempts, MDNS, SNTP, scan and the reconnect state
//   machine with bounded timeouts.
// - NOT: HTTP route registration, NVS persistence, SMTP/ZTE/modem dialogs and
//   the boot trace.
// INVARIANTS: Access-point mode respects initial-setup vs protected fallback;
// mDNS is restarted on every station connect; lastConnectionError is the
// single operator-facing failure reason and never holds credentials; DNS is
// served only while the AP is active.
// DEPENDENCIES: Uses Arduino-ESP32 WiFi, ESPmDNS, DNSServer, esp_mac and
// Preferences; reads RuntimeConfig for SSID/password and emits WebStatus for
// the UI.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_WIFI_MANAGER_H
#define SYSTEM_WIFI_MANAGER_H

#include <Arduino.h>
#include <DNSServer.h>

#include "persistence/config_store.h"
#include "system/web_api.h"

constexpr unsigned long kConnectTimeoutMs = 30UL * 1000UL;
constexpr unsigned long kReconnectIntervalMs = 60UL * 1000UL;
constexpr unsigned long kApShutdownDelayMs = 3UL * 1000UL;
constexpr unsigned long kWiFiTestStepMs = 100;
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kHttpPort = 80;

// #region CLASS_WifiManager
// PURPOSE: Encapsulates all Wi-Fi lifecycle state and operations so
// sms_gate.ino only drives setup/loop and delegates network behaviour.
class WifiManager {
 public:
  void initIdentity();
  void startMdns();
  void startAccessPoint(const RuntimeConfig& config);
  void stopAccessPoint();
  void beginStationAttempt(const RuntimeConfig& config);
  void startWallClock();
  void onStationConnected(bool deferAccessPointShutdown);
  void onStationFailed(const RuntimeConfig& config);
  WebStatus buildStatus(const RuntimeConfig& config) const;
  String buildScanNetworksJson();
  bool testStationCandidate(const RuntimeConfig& candidate, const RuntimeConfig& previous);
  void loop(const RuntimeConfig& config);
  void handleDns();

  const String& stationMacAddress() const { return stationMacAddress_; }
  const String& accessPointSsid() const { return accessPointSsid_; }
  const String& mdnsHostname() const { return mdnsHostname_; }
  const String& lastConnectionError() const { return lastConnectionError_; }
  void setLastConnectionError(const String& error) { lastConnectionError_ = error; }
  void scheduleAccessPointRestart();

  enum class ConnectionState { kInitialSetup, kConnecting, kOnline, kFallbackAp };
  ConnectionState connectionState() const { return connectionState_; }
  void setConnectionState(ConnectionState state) { connectionState_ = state; }
  bool accessPointActive() const { return accessPointActive_; }
  bool mdnsActive() const { return mdnsActive_; }

 private:
  // #region METHOD_buildStationMacAddress
  // PURPOSE: Reads the stable eFuse station MAC for the access-point whitelist
  // instead of relying on an uninitialised Wi-Fi interface.
  String buildStationMacAddress() const;
  // #endregion METHOD_buildStationMacAddress

  // #region METHOD_buildDeviceSuffix
  // PURPOSE: Produces a stable per-device suffix for the AP SSID and mDNS name.
  String buildDeviceSuffix() const;
  // #endregion METHOD_buildDeviceSuffix

  String stationMacAddress_;
  String accessPointSsid_;
  String mdnsHostname_;
  String lastConnectionError_;
  bool accessPointActive_ = false;
  bool mdnsActive_ = false;
  unsigned long connectionAttemptStartedAt_ = 0;
  unsigned long nextReconnectAt_ = 0;
  unsigned long accessPointShutdownAt_ = 0;
  unsigned long accessPointRestartAt_ = 0;
  ConnectionState connectionState_ = ConnectionState::kInitialSetup;
  DNSServer dnsServer_;
};
// #endregion CLASS_WifiManager
#endif  // SYSTEM_WIFI_MANAGER_H
