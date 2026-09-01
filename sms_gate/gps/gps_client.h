// #region MODULE_CONTRACT
// PURPOSE: Keeps GNSS parsing testable and hardware access bounded.
// SCOPE:
// - GNSS snapshots, AT dialog, fix/time conversion, and response parsers.
// - NOT: Hardware transport, persistence, HTTP rendering, or clock arbitration.
// INVARIANTS:
// - Dialogs leave the channel idle;
// - buffers stay bounded;
// - unknown modem sentinels remain distinguishable.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef GPS_GPS_CLIENT_H
#define GPS_GPS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

// #region ENUM_GpsResult
// PURPOSE: Keeps GNSS failures classifiable for logs and HTTP.
enum class GpsResult {
  kSuccess,
  kNotPresent,     // No AT OK within timeout
  kTimeout,        // Command timed out
  kProtocolError,  // Unexpected reply shape
};
// #endregion ENUM_GpsResult

// #region STRUCT_GpsSatsInfo
// PURPOSE: Preserves constellation detail for operator diagnosis.
struct GpsSatsInfo {
  int gps = 0;      // <GPS-SVs> visible
  int glonass = 0;  // <GLONASS-SVs> visible
  int galileo = 0;  // <GALILEO-SVs> visible
  int beidou = 0;   // <BEIDOU-SVs> visible
  int visible = 0;  // sum of the four constellation counts
  int used = 0;     // <NoSV>: satellites involved in positioning
};
// #endregion STRUCT_GpsSatsInfo

// #region STRUCT_GpsStatus
// PURPOSE: Gives consumers one bounded GNSS snapshot without live AT access.
struct GpsStatus {
  bool present = false;  // AT OK seen
  bool powered = false;  // +CGNSSPWR: 1
  bool fix = false;
  int mode = -1;     // +CGNSSMODE or -1 unknown
  GpsSatsInfo sats;  // counts from +CGNSSINFO
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

// Forward declaration avoids an include cycle; the definition lives in
// modem/modem_client.h and modem_transport.h reuses it.
class ModemChannel;

// #region CLASS_GpsClient
// PURPOSE: Keeps GNSS polling independent from hardware transport details.
class GpsClient {
 public:
  // #region METHOD_GpsClient_GpsClient
  // PURPOSE: Gives each GNSS dialog an isolated channel and workspace.
  GpsClient(ModemChannel& channel, char* scratch, size_t scratchSize);
  // #endregion METHOD_GpsClient_GpsClient

  // #region METHOD_GpsClient_restart
  // PURPOSE: Restores receiver state needed for fix acquisition.
  GpsResult restart();
  // #endregion METHOD_GpsClient_restart

  // #region METHOD_GpsClient_ensurePowered
  // PURPOSE: Ensures polling starts with a usable receiver state.
  GpsResult ensurePowered();
  // #endregion METHOD_GpsClient_ensurePowered

  // #region METHOD_GpsClient_poll
  // PURPOSE: Supplies the service with one bounded receiver snapshot.
  GpsResult poll(GpsStatus& out);
  // #endregion METHOD_GpsClient_poll

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
// #endregion CLASS_GpsClient

// #region FUNC_parseCgnssPwrLine
// PURPOSE: Lets polling track receiver availability from AT replies.
bool parseCgnssPwrLine(const char* line, bool& powered);
// #endregion FUNC_parseCgnssPwrLine

// #region FUNC_parseCgnssModeLine
// PURPOSE: Keeps receiver-mode reporting stable across AT replies.
bool parseCgnssModeLine(const char* line, int& mode);
// #endregion FUNC_parseCgnssModeLine

// #region STRUCT_GpsFixFields
// PURPOSE: Keeps parsed fix data consistent across GNSS consumers.
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
// #endregion STRUCT_GpsFixFields

// #region FUNC_parseCgpsInfoLine
// PURPOSE: Makes one receiver fix safe for status and clock consumers.
bool parseCgpsInfoLine(const char* line, GpsFixFields& out);
// #endregion FUNC_parseCgpsInfoLine
// Parses +CGNSSINFO by the SIM767xx manual V1.02 field order
// (<mode>,<GPS-SVs>,<GLONASS-SVs>,<GALILEO-SVs>,<BEIDOU-SVs>,...,<NoSV>) into
// per-constellation counts; field 0 is the fix mode (2=2D/3=3D) and is not
// part of the output because GpsStatus.mode is sourced from AT+CGNSSMODE?.
// #region FUNC_parseCgnssInfoLine
// PURPOSE: Preserves satellite detail for operator diagnosis.
bool parseCgnssInfoLine(const char* line, GpsSatsInfo& out);
// #endregion FUNC_parseCgnssInfoLine

// #region FUNC_gpsFixToIso
// PURPOSE: Gives status and email one interoperable fix timestamp.
bool gpsFixToIso(const GpsFixFields& fix, char* out, size_t outSize);
// #endregion FUNC_gpsFixToIso

// #region FUNC_gpsFixToEpochMs
// PURPOSE: Makes trusted GNSS time usable by clock arbitration.
bool gpsFixToEpochMs(const GpsFixFields& fix, int64_t& epochMsOut);
// #endregion FUNC_gpsFixToEpochMs

// #region FUNC_nmeaToDecimal
// PURPOSE: Makes receiver coordinates usable by status consumers.
double nmeaToDecimal(const char* nmea, char dir);
// #endregion FUNC_nmeaToDecimal

#endif  // GPS_GPS_CLIENT_H
