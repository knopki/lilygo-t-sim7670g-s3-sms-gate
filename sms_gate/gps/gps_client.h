// #region MODULE_CONTRACT
// PURPOSE: Host-testable GNSS dialog for the SIM7670G internal receiver.
// Extracts power state, fix, satellite counts, coordinates, altitude and
// UTC time over abstract ModemChannel so logic stays testable without
// hardware (mirrors modem_client.h style).
// SCOPE:
// - GpsStatus snapshot, GpsFix helpers, GpsResult stable outcome,
//   GpsClient ensurePowered/poll with scratch and failedStage,
//   pure parsers for +CGNSSPWR/+CGPSINFO/+CGNSSINFO/CGPSSAT.
// - NOT: HardwareSerial ownership and pin control (modem_transport.h),
//   NVS persistence, HTTP/JSON rendering and system clock sync.
// INVARIANTS: Every public method leaves channel idle on return;
// credentials never appear in stage names; parsing tolerates empty
// fields and 99/255 sentinels; all public entities have GRACE contracts.
// DEPENDENCIES: Pure C++; device channel lives in modem_transport.h;
// tests use FakeModemChannel.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef GPS_GPS_CLIENT_H
#define GPS_GPS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

// #region ENUM_GpsResult
// PURPOSE: Stable outcome per failure class for Serial events and HTTP.
enum class GpsResult {
  kSuccess,
  kNotPresent,     // No AT OK within timeout
  kTimeout,        // Command timed out
  kProtocolError,  // Unexpected reply shape
};
// #endregion ENUM_GpsResult

// #region STRUCT_GpsStatus
// PURPOSE: Snapshot for the portMUX cache and GET /api/gps/status. All strings
// are bounded NUL-terminated buffers; lat/lon are decimal degrees, 0 when no fix.
struct GpsStatus {
  bool present = false;  // AT OK seen
  bool powered = false;  // +CGNSSPWR: 1
  bool fix = false;
  int mode = -1;  // +CGNSSMODE or -1 unknown
  int satsUsed = 0;
  int satsVisible = 0;
  double lat = 0.0;
  double lon = 0.0;
  float alt = 0.0f;
  float speed = 0.0f;
  float course = 0.0f;
  char date[16] = "";     // ddmmyy or yyyy-mm-dd
  char utcTime[16] = "";  // hhmmss
  char isoTime[32] = "";  // YYYY-MM-DDThh:mm:ssZ when fix has time
  int timeMs = 0;         // 0..999 fractional part of hhmmss.s
  uint32_t updatedMs = 0;
  char lastNmeaLat[24] = "";
  char lastNmeaLon[24] = "";
};
// #endregion STRUCT_GpsStatus

// #region CLASS_ModemChannel_fwd
// PURPOSE: Forward declare ModemChannel to avoid include cycle; real
// definition lives in modem/modem_client.h and modem_transport.h reuses it.
class ModemChannel;
class GpsClient {
 public:
  GpsClient(ModemChannel& channel, char* scratch, size_t scratchSize);

  GpsResult ensurePowered();
  GpsResult poll(GpsStatus& out);

  const char* failedStage() const { return failedStage_; }
  const char* lastReply() const { return scratch_ ? scratch_ : ""; }

 private:
  GpsResult sendCommand(const char* cmd, unsigned long timeoutMs);
  GpsResult readResponse();
  void fail(const char* stage);

  ModemChannel& channel_;
  char* scratch_;
  size_t scratchSize_;
  const char* failedStage_ = "";
  size_t replyLen_ = 0;
};
// #endregion CLASS_ModemChannel_fwd

// Pure parsers exposed for host tests.
bool parseCgnssPwrLine(const char* line, bool& powered);
bool parseCgnssModeLine(const char* line, int& mode);
struct GpsFixFields {
  double lat = 0.0;
  double lon = 0.0;
  char latDir = '\0';
  char lonDir = '\0';
  char date[16] = "";
  char utcTime[16] = "";
  int timeMs = 0;  // 0..999 fractional part of hhmmss.s
  float alt = 0.0f;
  float speed = 0.0f;
  float course = 0.0f;
  bool hasFix = false;
  char rawLat[24] = "";
  char rawLon[24] = "";
};
bool parseCgpsInfoLine(const char* line, GpsFixFields& out);
bool parseCgnssInfoLine(const char* line, int& mode, int& satsUsed, int& satsVisible);
bool gpsFixToIso(const GpsFixFields& fix, char* out, size_t outSize);
// Converts fix date+time (with ms) to UTC epoch milliseconds; false on malformed.
bool gpsFixToEpochMs(const GpsFixFields& fix, int64_t& epochMsOut);
double nmeaToDecimal(const char* nmea, char dir);

#endif  // GPS_GPS_CLIENT_H
