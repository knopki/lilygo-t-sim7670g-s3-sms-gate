// #region MODULE_CONTRACT
// PURPOSE: Keeps GNSS wire behavior testable before hardware I/O.
// SCOPE:
//   - Parses GNSS modem replies and implements bounded GNSS command dialogs.
//   - NOT: Persisting GNSS policy or scheduling service tasks.
// INVARIANTS:
//   - Malformed modem replies do not produce accepted GNSS state.
//   - Parsed timestamps and coordinates are converted before leaving the client boundary.
// #endregion MODULE_CONTRACT

#include "gps/gps_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modem/modem_client.h"
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
inline void vTaskDelay(unsigned long) {}
#define pdMS_TO_TICKS(x) (x)
#endif

namespace {
constexpr unsigned long kGpsDefaultTimeoutMs = 1500;
constexpr unsigned long kGpsPowerTimeoutMs = 5000;
constexpr int kGpsMaxLines = 20;

// #region FUNC_parseGpsDateTime
// PURPOSE: Rejects malformed calendar values before they can become a GNSS clock sample.
bool parseGpsDateTime(const char* date, const char* utcTime, int& year, int& month, int& day,
                      int& hour, int& minute, int& second) {
  if (date == nullptr || utcTime == nullptr || strlen(date) != 6 || strlen(utcTime) != 6) {
    return false;
  }
  for (size_t i = 0; i < 6; ++i) {
    if (!isdigit(static_cast<unsigned char>(date[i])) ||
        !isdigit(static_cast<unsigned char>(utcTime[i]))) {
      return false;
    }
  }
  day = (date[0] - '0') * 10 + date[1] - '0';
  month = (date[2] - '0') * 10 + date[3] - '0';
  year = (date[4] - '0') * 10 + date[5] - '0';
  year += year < 70 ? 2000 : 1900;
  hour = (utcTime[0] - '0') * 10 + utcTime[1] - '0';
  minute = (utcTime[2] - '0') * 10 + utcTime[3] - '0';
  second = (utcTime[4] - '0') * 10 + utcTime[5] - '0';
  if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 60) return false;

  const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int daysInMonth[] = {0, 31, leapYear ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return day >= 1 && day <= daysInMonth[month];
}
// #endregion FUNC_parseGpsDateTime

// #region FUNC_isValidNmeaCoordinate
// PURPOSE: Rejects malformed GNSS positions before they can publish a false fix.
bool isValidNmeaCoordinate(const char* nmea, char direction, int maxDegrees) {
  const bool isLatitude = maxDegrees == 90;
  if (nmea == nullptr || nmea[0] == '\0' ||
      (isLatitude
           ? !(direction == 'N' || direction == 'S' || direction == 'n' || direction == 's')
           : !(direction == 'E' || direction == 'W' || direction == 'e' || direction == 'w'))) {
    return false;
  }
  char* end = nullptr;
  double raw = strtod(nmea, &end);
  if (end == nmea || *end != '\0' || !(raw >= 0.0) || raw > maxDegrees * 100.0 + 60.0) {
    return false;
  }
  const int degrees = static_cast<int>(raw / 100.0);
  const double minutes = raw - degrees * 100.0;
  return minutes >= 0.0 && minutes < 60.0 &&
         (degrees < maxDegrees || (degrees == maxDegrees && minutes == 0.0));
}
// #endregion FUNC_isValidNmeaCoordinate
}  // namespace

// #region FUNC_parseCgnssPwrLine
// PURPOSE: Keeps GNSS power decisions strict when modem replies are malformed.
bool parseCgnssPwrLine(const char* line, bool& powered) {
  const char* p = strstr(line, "+CGNSSPWR:");
  if (p == nullptr) return false;
  p += 10;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '1') {
    powered = true;
    return true;
  }
  if (*p == '0') {
    powered = false;
    return true;
  }
  return false;
}
// #endregion FUNC_parseCgnssPwrLine

// #region FUNC_parseCgnssModeLine
// PURPOSE: Keeps receiver-mode decisions strict when modem replies are malformed.
bool parseCgnssModeLine(const char* line, int& mode) {
  const char* p = strstr(line, "+CGNSSMODE:");
  if (p == nullptr) return false;
  p += 11;
  while (*p == ' ' || *p == '\t') ++p;
  char* end = nullptr;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  mode = static_cast<int>(v);
  return true;
}
// #endregion FUNC_parseCgnssModeLine

// #region FUNC_nmeaToDecimal
// PURPOSE: Makes modem coordinates usable without carrying NMEA formatting downstream.
double nmeaToDecimal(const char* nmea, char dir) {
  if (nmea == nullptr || nmea[0] == '\0') return 0.0;
  char* end = nullptr;
  const double raw = strtod(nmea, &end);
  if (end == nmea || *end != '\0') return 0.0;
  const int deg = static_cast<int>(raw / 100);
  const double minutes = raw - deg * 100.0;
  double decimal = deg + minutes / 60.0;
  if (dir == 'S' || dir == 'W' || dir == 's' || dir == 'w') decimal = -decimal;
  return decimal;
}
// #endregion FUNC_nmeaToDecimal

// #region FUNC_gpsFixToEpochMs
// PURPOSE: Turns a valid GNSS timestamp into epoch time for source arbitration.
bool gpsFixToEpochMs(const GpsFixFields& fix, int64_t& epochMsOut) {
  int year = 0, mon = 0, day = 0, h = 0, m = 0, s = 0;
  if (!parseGpsDateTime(fix.date, fix.utcTime, year, mon, day, h, m, s)) return false;
  if (fix.timeMs < 0 || fix.timeMs > 999) return false;
  auto daysFromCivil = [](int y, int mo, int d) -> int64_t {
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + doe - 719468LL;
  };
  int64_t days = daysFromCivil(year, mon, day);
  int64_t epochSec = days * 86400LL + h * 3600LL + m * 60LL + s;
  epochMsOut = epochSec * 1000LL + fix.timeMs;
  return true;
}
// #endregion FUNC_gpsFixToEpochMs

// #region FUNC_gpsFixToIso
// PURPOSE: Keeps GNSS timestamps consistent across status and forwarded email output.
bool gpsFixToIso(const GpsFixFields& fix, char* out, size_t outSize) {
  int year = 0, mon = 0, day = 0, h = 0, m = 0, s = 0;
  if (!parseGpsDateTime(fix.date, fix.utcTime, year, mon, day, h, m, s)) return false;
  snprintf(out, outSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, mon, day, h, m, s);
  return true;
}
// #endregion FUNC_gpsFixToIso

// #region FUNC_parseCgpsInfoLine
// PURPOSE: Turns optional CGPSINFO data into a bounded fix snapshot, preserving no-fix state.
bool parseCgpsInfoLine(const char* line, GpsFixFields& out) {
  out = GpsFixFields{};
  const char* p = strstr(line, "+CGPSINFO:");
  if (p == nullptr) return false;
  p += 10;
  while (*p == ' ' || *p == '\t') ++p;
  // Copy payload to mutable buffer for tokenization
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", p);
  // Trim trailing CR
  size_t bl = strlen(buf);
  while (bl > 0 && (buf[bl - 1] == '\r' || buf[bl - 1] == '\n' || buf[bl - 1] == ' '))
    buf[--bl] = '\0';
  if (bl == 0) {
    // empty payload
  }
  // Early empty detection: ",,,," or payload starts with comma
  bool allEmpty = true;
  for (size_t i = 0; i < bl; ++i) {
    if (buf[i] != ',' && buf[i] != ' ' && buf[i] != '\t') {
      allEmpty = false;
      break;
    }
  }
  if (allEmpty) {
    out.hasFix = false;
    return true;
  }
  // Tokenize by commas preserving empties
  char* tokens[9] = {nullptr};
  int idx = 0;
  char* cur = buf;
  tokens[idx++] = cur;
  for (char* q = buf; *q && idx < 9; ++q) {
    if (*q == ',') {
      *q = '\0';
      if (idx < 9) tokens[idx++] = q + 1;
    }
  }
  // cppcheck-suppress cstyleCast
  while (idx < 9) tokens[idx++] = (char*)"";
  // Trim each token
  for (int i = 0; i < 9; ++i) {
    char* t = tokens[i];
    while (*t == ' ' || *t == '\t') ++t;
    size_t tl = strlen(t);
    while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) t[--tl] = '\0';
    tokens[i] = t;
  }
  char* latStr = tokens[0];
  char* latDirStr = tokens[1];
  char* lonStr = tokens[2];
  char* lonDirStr = tokens[3];
  char* dateStr = tokens[4];
  char* timeStr = tokens[5];
  char* altStr = tokens[6];
  char* speedStr = tokens[7];
  char* courseStr = tokens[8];
  int ms = 0;
  char* dot = strchr(timeStr, '.');
  if (dot != nullptr) {
    *dot = '\0';
    const char* frac = dot + 1;
    if (*frac == '\0') {
      out.hasFix = false;
      return true;
    }
    for (const char* digit = frac; *digit != '\0'; ++digit) {
      if (!isdigit(static_cast<unsigned char>(*digit))) {
        out.hasFix = false;
        return true;
      }
    }
    char msBuf[4] = "000";
    const size_t fracLength = strlen(frac);
    memcpy(msBuf, frac, fracLength < 3 ? fracLength : 3);
    ms = (msBuf[0] - '0') * 100 + (msBuf[1] - '0') * 10 + msBuf[2] - '0';
  }

  const char latDir = latDirStr[0];
  const char lonDir = lonDirStr[0];
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (latDirStr[1] != '\0' || lonDirStr[1] != '\0' || !isValidNmeaCoordinate(latStr, latDir, 90) ||
      !isValidNmeaCoordinate(lonStr, lonDir, 180) ||
      !parseGpsDateTime(dateStr, timeStr, year, month, day, hour, minute, second)) {
    out.hasFix = false;
    return true;
  }
  out.hasFix = true;
  snprintf(out.rawLat, sizeof(out.rawLat), "%s", latStr);
  snprintf(out.rawLon, sizeof(out.rawLon), "%s", lonStr);
  out.latDir = latDir;
  out.lonDir = lonDir;
  out.lat = nmeaToDecimal(latStr, out.latDir);
  out.lon = nmeaToDecimal(lonStr, out.lonDir);
  snprintf(out.date, sizeof(out.date), "%s", dateStr);
  snprintf(out.utcTime, sizeof(out.utcTime), "%s", timeStr);
  out.timeMs = ms;
  if (altStr[0]) out.alt = static_cast<float>(atof(altStr));
  if (speedStr[0]) out.speed = static_cast<float>(atof(speedStr));
  if (courseStr[0]) out.course = static_cast<float>(atof(courseStr));
  return true;
}
// #endregion FUNC_parseCgpsInfoLine

// #region FUNC_parseCgnssInfoLine
// PURPOSE: Preserves positional constellation fields so status uses the right satellite counts.
// The *-SVs fields are visible satellites per constellation; NoSV counts the
// satellites involved in positioning. Empty fields remain positional.
bool parseCgnssInfoLine(const char* line, GpsSatsInfo& out) {
  const char* p = strstr(line, "+CGNSSINFO");
  if (p == nullptr) return false;
  const char* colon = strchr(p, ':');
  if (colon == nullptr) return false;
  p = colon + 1;
  while (*p == ' ' || *p == '\t') ++p;

  char buf[192];
  const char* fields[18];
  int last = 0;  // index of the field currently being copied
  size_t n = 0;  // bytes written to buf
  fields[0] = buf;
  for (const char* q = p; *q != '\0' && *q != '\r' && *q != '\n'; ++q) {
    if (*q == ',' && last < 17) {
      buf[n++] = '\0';
      fields[++last] = buf + n;
    } else if (n + 1 < sizeof(buf)) {
      buf[n++] = *q;
    }
  }
  buf[n] = '\0';
  const int count = last + 1;

  out = GpsSatsInfo{};
  int* perConstellation[4] = {&out.gps, &out.glonass, &out.galileo, &out.beidou};
  for (int i = 1; i <= 4 && i < count; ++i) {
    const int value = atoi(fields[i]);
    if (value > 0) *perConstellation[i - 1] = value;
  }
  out.visible = out.gps + out.glonass + out.galileo + out.beidou;
  if (count >= 18) {
    const int used = atoi(fields[17]);
    out.used = used > 0 ? used : 0;
  }
  return true;
}
// #endregion FUNC_parseCgnssInfoLine

// #region METHOD_GpsClient_GpsClient
// PURPOSE: Binds one GNSS dialog instance to a channel and scratch buffer.
GpsClient::GpsClient(ModemChannel& channel, char* scratch, size_t scratchSize)
    : channel_(channel), scratch_(scratch), scratchSize_(scratchSize) {}
// #endregion METHOD_GpsClient_GpsClient

// #region METHOD_GpsClient_fail
// PURPOSE: Stores a stable failure stage without exposing modem data.
void GpsClient::fail(const char* stage) { failedStage_ = stage; }
// #endregion METHOD_GpsClient_fail

// #region METHOD_GpsClient_sendCommand
// PURPOSE: Starts each GNSS command from a clean channel boundary.
GpsResult GpsClient::sendCommand(const char* cmd, unsigned long timeoutMs) {
  (void)timeoutMs;
  channel_.purge();
  size_t len = strlen(cmd);
  char withCr[96];
  if (len + 2 >= sizeof(withCr)) {
    fail("cmd_too_long");
    return GpsResult::kProtocolError;
  }
  memcpy(withCr, cmd, len);
  withCr[len++] = '\r';
  withCr[len++] = '\n';
  if (!channel_.write(withCr, len)) {
    fail("write_failed");
    return GpsResult::kTimeout;
  }
  return GpsResult::kSuccess;
}
// #endregion METHOD_GpsClient_sendCommand

// #region METHOD_GpsClient_readResponse
// PURPOSE: Collects bounded GNSS replies for parser and status updates.
GpsResult GpsClient::readResponse() {
  if (scratch_ == nullptr || scratchSize_ == 0) {
    fail("no_scratch");
    return GpsResult::kProtocolError;
  }
  scratch_[0] = '\0';
  replyLen_ = 0;
  char line[192];
  for (int i = 0; i < kGpsMaxLines; ++i) {
    int len = channel_.readLine(line, sizeof(line), kGpsDefaultTimeoutMs);
    if (len < 0) {
      fail("timeout");
      return GpsResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") != 0) {
      snprintf(scratch_, scratchSize_, "%s", line);
      replyLen_ = strlen(scratch_);
    }
    if (strcmp(line, "OK") == 0) return GpsResult::kSuccess;
    if (strstr(line, "ERROR") != nullptr) {
      fail("modem_error");
      return GpsResult::kProtocolError;
    }
  }
  fail("no_ok");
  return GpsResult::kTimeout;
}
// #endregion METHOD_GpsClient_readResponse

// #region METHOD_GpsClient_restart
// PURPOSE: Forces one clean GNSS power cycle so antenna bias is established
// before receiver startup even when CGNSSPWR survived the previous ESP image.
GpsResult GpsClient::restart() {
  if (sendCommand("AT+CGNSSPWR?", kGpsDefaultTimeoutMs) != GpsResult::kSuccess) {
    fail("cgnsspwr_query");
    return GpsResult::kTimeout;
  }
  GpsResult queryResult = readResponse();
  if (queryResult != GpsResult::kSuccess) {
    fail("cgnsspwr_query");
    return queryResult;
  }

  bool powered = false;
  if (!parseCgnssPwrLine(scratch_, powered)) {
    fail("cgnsspwr_query");
    return GpsResult::kProtocolError;
  }
  if (powered) {
    if (sendCommand("AT+CGNSSPWR=0", kGpsPowerTimeoutMs) != GpsResult::kSuccess) {
      fail("cgnsspwr_off");
      return GpsResult::kTimeout;
    }
    GpsResult powerOffResult = readResponse();
    if (powerOffResult != GpsResult::kSuccess) {
      fail("cgnsspwr_off");
      return powerOffResult;
    }
  }
  return ensurePowered();
}
// #endregion METHOD_GpsClient_restart

// #region METHOD_GpsClient_ensurePowered
// PURPOSE: Makes GNSS power state explicit before a poll.
GpsResult GpsClient::ensurePowered() {
  // This firmware targets the Classic board, whose active GNSS antenna is wired
  // to modem GPIO4. GPIO1 commands can return OK but only drive the Standard
  // board's antenna route, so an OK response cannot be used for auto-detection.
  if (sendCommand("AT+CGDRT=4,1", 2000) != GpsResult::kSuccess) {
    fail("antenna_bias_route");
    return GpsResult::kTimeout;
  }
  GpsResult antennaRoute = readResponse();
  if (antennaRoute != GpsResult::kSuccess) {
    fail("antenna_bias_route");
    return antennaRoute;
  }
  if (sendCommand("AT+CGSETV=4,1", 2000) != GpsResult::kSuccess) {
    fail("antenna_bias_voltage");
    return GpsResult::kTimeout;
  }
  GpsResult antennaVoltage = readResponse();
  if (antennaVoltage != GpsResult::kSuccess) {
    fail("antenna_bias_voltage");
    return antennaVoltage;
  }

  // Check current power only after restoring antenna bias. The modem can keep
  // CGNSSPWR=1 across an ESP/task restart while the antenna route is not set.
  if (sendCommand("AT+CGNSSPWR?", kGpsDefaultTimeoutMs) == GpsResult::kSuccess) {
    GpsResult r = readResponse();
    if (r == GpsResult::kSuccess) {
      bool p = false;
      if (parseCgnssPwrLine(scratch_, p) && p) {
        // Already powered
      } else {
        // Need power on sequence
        goto power_on;
      }
    } else {
      goto power_on;
    }
    // Ensure mode is 3 (GPS+GLONASS+BeiDou) if already powered
    if (sendCommand("AT+CGNSSMODE?", kGpsDefaultTimeoutMs) == GpsResult::kSuccess) {
      GpsResult rm = readResponse();
      if (rm == GpsResult::kSuccess) {
        int mode = -1;
        if (parseCgnssModeLine(scratch_, mode) && mode == 3) {
          return GpsResult::kSuccess;
        }
      }
    }
    if (sendCommand("AT+CGNSSMODE=3", kGpsDefaultTimeoutMs) == GpsResult::kSuccess) readResponse();
    return GpsResult::kSuccess;
  }
power_on:
  if (sendCommand("AT+CGNSSPWR=1", kGpsPowerTimeoutMs) == GpsResult::kSuccess) {
    GpsResult r = readResponse();
    if (r != GpsResult::kSuccess) {
      fail("cgnsspwr");
      return r;
    }
  } else {
    fail("cgnsspwr");
    return GpsResult::kTimeout;
  }
  // Firmware needs 1.5-2s after CGNSSPWR=1 before next AT is reliable (probe kGnssPowerOnWaitMs).
  vTaskDelay(pdMS_TO_TICKS(2000));
  if (sendCommand("AT+CGNSSMODE=3", 2000) == GpsResult::kSuccess) readResponse();
  return GpsResult::kSuccess;
}
// #endregion METHOD_GpsClient_ensurePowered

// #region METHOD_GpsClient_poll
// PURPOSE: Produces one complete GNSS snapshot for the service.
GpsResult GpsClient::poll(GpsStatus& out) {
  GpsStatus tmp;
  tmp.present = false;
  tmp.powered = false;
  // AT liveness
  if (sendCommand("AT", kGpsDefaultTimeoutMs) != GpsResult::kSuccess) {
    out = tmp;
    fail("at");
    return GpsResult::kNotPresent;
  }
  if (readResponse() != GpsResult::kSuccess) {
    out = tmp;
    fail("at");
    return GpsResult::kNotPresent;
  }
  tmp.present = true;

  // Ensure GNSS engine powered (also sets mode)
  GpsResult pr = ensurePowered();
  if (pr != GpsResult::kSuccess) {
    out = tmp;
    return pr;
  }
  tmp.powered = true;

  // CGNSSPWR? echo check
  if (sendCommand("AT+CGNSSPWR?", kGpsDefaultTimeoutMs) == GpsResult::kSuccess &&
      readResponse() == GpsResult::kSuccess) {
    bool p = false;
    if (parseCgnssPwrLine(scratch_, p)) tmp.powered = p;
  }
  if (sendCommand("AT+CGNSSMODE?", kGpsDefaultTimeoutMs) == GpsResult::kSuccess &&
      readResponse() == GpsResult::kSuccess) {
    int m = -1;
    if (parseCgnssModeLine(scratch_, m)) tmp.mode = m;
  }
  // CGPSINFO
  if (sendCommand("AT+CGPSINFO", 3000) == GpsResult::kSuccess &&
      readResponse() == GpsResult::kSuccess) {
    GpsFixFields fix{};
    if (parseCgpsInfoLine(scratch_, fix)) {
      tmp.fix = fix.hasFix;
      if (fix.hasFix) {
        tmp.lat = fix.lat;
        tmp.lon = fix.lon;
        tmp.alt = fix.alt;
        tmp.speed = fix.speed;
        tmp.course = fix.course;
        snprintf(tmp.date, sizeof(tmp.date), "%s", fix.date);
        snprintf(tmp.utcTime, sizeof(tmp.utcTime), "%s", fix.utcTime);
        tmp.timeMs = fix.timeMs;
        snprintf(tmp.lastNmeaLat, sizeof(tmp.lastNmeaLat), "%s", fix.rawLat);
        snprintf(tmp.lastNmeaLon, sizeof(tmp.lastNmeaLon), "%s", fix.rawLon);
        gpsFixToIso(fix, tmp.isoTime, sizeof(tmp.isoTime));
      }
    }
  }
  // CGNSSINFO for sats
  if (sendCommand("AT+CGNSSINFO", 3000) == GpsResult::kSuccess &&
      readResponse() == GpsResult::kSuccess) {
    GpsSatsInfo sats;
    if (parseCgnssInfoLine(scratch_, sats)) {
      tmp.sats = sats;
    }
  }
  out = tmp;
  return GpsResult::kSuccess;
}
// #endregion METHOD_GpsClient_poll
