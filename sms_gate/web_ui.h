// #region MODULE_CONTRACT
// PURPOSE: Separates HTML rendering for the local configuration interface from
// Wi-Fi state management and HTTP route handling.
// SCOPE: Escaping values and rendering setup/configuration documents.
// NOT: Authentication, form handling, Wi-Fi scans, or persistence.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

#include "config_store.h"

struct WebStatus {
  String mode;
  String stationIp;
  String macAddress;
  String rssi;
  String mdnsHostname;
  String lastError;
};

String escapeHtml(const String& value);
String renderSetupPage(const String& networkOptions, const String& error,
                       const String& stationMacAddress);
String renderConfigPage(const RuntimeConfig& config, const WebStatus& status,
                        const String& networkOptions, const String& message, const String& error);
