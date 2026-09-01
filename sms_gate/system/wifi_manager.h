// #region MODULE_CONTRACT
// PURPOSE: Keeps network recovery and device identity stable across provisioning.
// SCOPE:
// - Owns AP/STA transitions, identity, mDNS, DNS, scanning, and retries.
// - NOT: HTTP routes, persistence, protocol dialogs, or boot tracing.
// INVARIANTS:
// - Fallback AP is protected;
// - mDNS follows station connection;
// - DNS is served only while AP is active;
// - errors never contain credentials.
// DEPENDENCIES: WiFi, ESPmDNS, DNSServer, esp_mac, Preferences, WebStatus.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_WIFI_MANAGER_H
#define SYSTEM_WIFI_MANAGER_H

#include <Arduino.h>
#include <DNSServer.h>

#include "persistence/config_store_network.h"
#include "system/time_sync.h"
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
  // #region METHOD_WifiManager_initIdentity
  // PURPOSE: Establishes stable per-device names before networking starts.
  void initIdentity();
  // #endregion METHOD_WifiManager_initIdentity

  // #region METHOD_WifiManager_startMdns
  // PURPOSE: Publishes the device name after station networking is ready.
  void startMdns();
  // #endregion METHOD_WifiManager_startMdns

  // #region METHOD_WifiManager_startAccessPoint
  // PURPOSE: Provides provisioning or recovery access when STA is unavailable.
  void startAccessPoint(const RuntimeConfig& config);
  // #endregion METHOD_WifiManager_startAccessPoint

  // #region METHOD_WifiManager_stopAccessPoint
  // PURPOSE: Releases AP resources after station service is established.
  void stopAccessPoint();
  // #endregion METHOD_WifiManager_stopAccessPoint

  // #region METHOD_WifiManager_beginStationAttempt
  // PURPOSE: Tests saved access without blocking provisioning or recovery services.
  void beginStationAttempt(const RuntimeConfig& config);
  // #endregion METHOD_WifiManager_beginStationAttempt

  // #region METHOD_WifiManager_startWallClock
  // PURPOSE: Starts configured SNTP fallback when station service is online.
  void startWallClock(const RuntimeConfig& config);
  // #endregion METHOD_WifiManager_startWallClock

  // #region METHOD_WifiManager_onStationConnected
  // PURPOSE: Completes the online transition and refreshes dependent services.
  void onStationConnected(const RuntimeConfig& config, bool deferAccessPointShutdown);
  // #endregion METHOD_WifiManager_onStationConnected

  // #region METHOD_WifiManager_onStationFailed
  // PURPOSE: Records failure and schedules protected fallback recovery.
  void onStationFailed(const RuntimeConfig& config);
  // #endregion METHOD_WifiManager_onStationFailed
  void setTimeSync(TimeSync* timeSync) { timeSync_ = timeSync; }
  // #region METHOD_WifiManager_buildStatus
  // PURPOSE: Projects network state into the operator API snapshot.
  WebStatus buildStatus(const RuntimeConfig& config) const;
  // #endregion METHOD_WifiManager_buildStatus

  // #region METHOD_WifiManager_buildScanNetworksJson
  // PURPOSE: Gives provisioning a bounded network list without exposing scan internals.
  String buildScanNetworksJson();
  // #endregion METHOD_WifiManager_buildScanNetworksJson

  // #region METHOD_WifiManager_testStationCandidate
  // PURPOSE: Verifies replacement credentials before they are persisted.
  bool testStationCandidate(const RuntimeConfig& candidate, const RuntimeConfig& previous);
  // #endregion METHOD_WifiManager_testStationCandidate

  // #region METHOD_WifiManager_loop
  // PURPOSE: Advances STA/AP recovery without blocking other services.
  void loop(const RuntimeConfig& config);
  // #endregion METHOD_WifiManager_loop

  // #region METHOD_WifiManager_handleDns
  // PURPOSE: Keeps captive-portal resolution available while AP is active.
  void handleDns();
  // #endregion METHOD_WifiManager_handleDns

  const String& stationMacAddress() const { return stationMacAddress_; }
  const String& accessPointSsid() const { return accessPointSsid_; }
  const String& mdnsHostname() const { return mdnsHostname_; }
  const String& lastConnectionError() const { return lastConnectionError_; }
  void setLastConnectionError(const String& error) { lastConnectionError_ = error; }
  // #region METHOD_WifiManager_scheduleAccessPointRestart
  // PURPOSE: Schedules a protected AP restart after station recovery fails.
  void scheduleAccessPointRestart();
  // #endregion METHOD_WifiManager_scheduleAccessPointRestart

  // #region ENUM_ConnectionState
  // PURPOSE: Names the Wi-Fi lifecycle states used by recovery logic.
  enum class ConnectionState { kInitialSetup, kConnecting, kOnline, kFallbackAp };
  // #endregion ENUM_ConnectionState
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

  TimeSync* timeSync_ = nullptr;
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
