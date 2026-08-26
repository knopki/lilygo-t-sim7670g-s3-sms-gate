// #region MODULE_CONTRACT
// PURPOSE: Implements host-testable GNSS AT dialog for SIM7670G (CGNSS).
// #endregion MODULE_CONTRACT

#include "gps/gps_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modem/modem_client.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr unsigned long kGpsDefaultTimeoutMs = 1500;
constexpr unsigned long kGpsPowerTimeoutMs = 5000;
constexpr int kGpsMaxLines = 20;
}  // namespace

// #region FUNC_parseCgnssPwrLine
// PURPOSE: Parses +CGNSSPWR: 1/0.
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
// PURPOSE: Parses +CGNSSMODE: <mode>.
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
// PURPOSE: Converts NMEA ddmm.mmmm + dir (N/S/E/W) to decimal degrees.
double nmeaToDecimal(const char* nmea, char dir) {
  if (nmea == nullptr || nmea[0] == '\0') return 0.0;
  double raw = atof(nmea);
  int deg = static_cast<int>(raw / 100);
  double minutes = raw - deg * 100.0;
  double decimal = deg + minutes / 60.0;
  if (dir == 'S' || dir == 'W' || dir == 's' || dir == 'w') decimal = -decimal;
  return decimal;
}
// #endregion FUNC_nmeaToDecimal

// #region FUNC_gpsFixToIso
// PURPOSE: Converts fix date (ddmmyy) + time (hhmmss.s) to ISO8601 UTC.
// Returns false when fields are empty or malformed.
bool gpsFixToIso(const GpsFixFields& fix, char* out, size_t outSize) {
  if (fix.date[0] == '\0' || fix.utcTime[0] == '\0') return false;
  if (strlen(fix.date) != 6) return false;
  // cppcheck-suppress constVariable
  char dd[3] = {fix.date[0], fix.date[1], '\0'};
  // cppcheck-suppress constVariable
  char mm[3] = {fix.date[2], fix.date[3], '\0'};
  // cppcheck-suppress constVariable
  char yy[3] = {fix.date[4], fix.date[5], '\0'};
  int day = atoi(dd);
  int mon = atoi(mm);
  int year = atoi(yy);
  if (day < 1 || day > 31 || mon < 1 || mon > 12) return false;
  year += (year < 70 ? 2000 : 1900);
  // time: hhmmss[.s] — take first 6 chars
  if (strlen(fix.utcTime) < 6) return false;
  // cppcheck-suppress constVariable
  char hh[3] = {fix.utcTime[0], fix.utcTime[1], '\0'};
  // cppcheck-suppress constVariable
  char mi[3] = {fix.utcTime[2], fix.utcTime[3], '\0'};
  // cppcheck-suppress constVariable
  char ss[3] = {fix.utcTime[4], fix.utcTime[5], '\0'};
  int h = atoi(hh), m = atoi(mi), s = atoi(ss);
  if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 60) return false;
  snprintf(out, outSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, mon, day, h, m, s);
  return true;
}
// #endregion FUNC_gpsFixToIso

// #region FUNC_parseCgpsInfoLine
// PURPOSE: Parses +CGPSINFO: <lat>,<N/S>,<lon>,<E/W>,<date>,<time>,<alt>,<speed>,<course>
// Empty fields (,,,,) => no fix.
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
  if (latStr[0] == '\0' || lonStr[0] == '\0') {
    out.hasFix = false;
    return true;
  }
  out.hasFix = true;
  snprintf(out.rawLat, sizeof(out.rawLat), "%s", latStr);
  snprintf(out.rawLon, sizeof(out.rawLon), "%s", lonStr);
  out.latDir = latDirStr[0] ? latDirStr[0] : 'N';
  out.lonDir = lonDirStr[0] ? lonDirStr[0] : 'E';
  out.lat = nmeaToDecimal(latStr, out.latDir);
  out.lon = nmeaToDecimal(lonStr, out.lonDir);
  snprintf(out.date, sizeof(out.date), "%s", dateStr);
  // time may contain decimal part -> strip after '.'
  {
    char* dot = strchr(timeStr, '.');
    if (dot) *dot = '\0';
    snprintf(out.utcTime, sizeof(out.utcTime), "%s", timeStr);
  }
  if (altStr[0]) out.alt = static_cast<float>(atof(altStr));
  if (speedStr[0]) out.speed = static_cast<float>(atof(speedStr));
  if (courseStr[0]) out.course = static_cast<float>(atof(courseStr));
  return true;
}
// #endregion FUNC_parseCgpsInfoLine

// #region FUNC_parseCgnssInfoLine
// PURPOSE: Parses +CGNSSINFO: <mode>,<fix>,<satsVisible?>,<satsUsed?> flexible.
// SIM7670G variants emit mode, fix, totalVisible, fixSats... Keep first numeric fields.
bool parseCgnssInfoLine(const char* line, int& mode, int& satsUsed, int& satsVisible) {
  const char* p = strstr(line, "+CGNSSINFO:");
  if (p == nullptr) p = strstr(line, "+CGNSSINFO");
  if (p == nullptr) return false;
  const char* colon = strchr(p, ':');
  if (colon == nullptr) return false;
  p = colon + 1;
  while (*p == ' ' || *p == '\t') ++p;
  // Extract up to 4 ints
  char buf[160];
  snprintf(buf, sizeof(buf), "%s", p);
  // Replace non digit/comma/minus with comma? Simple sscanf attempt
  int m = -1, fix = -1, vis = 0, used = 0;
  // Try: mode,fix,visible,used  OR mode,sats, ...
  // Use strtok
  char* tok = strtok(buf, ",");
  int idx = 0;
  int vals[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  while (tok && idx < 8) {
    while (*tok == ' ' || *tok == '\t') ++tok;
    if (*tok == '\0')
      vals[idx] = 0;
    else
      vals[idx] = atoi(tok);
    vals[idx] = vals[idx] < 0 ? 0 : vals[idx];
    tok = strtok(nullptr, ",");
    ++idx;
  }
  if (idx < 1) return false;
  m = vals[0];
  // Heuristic: SIM7670G spec: +CGNSSINFO: <mode>,<sats>,... where sats is used count?
  // We'll map vals[1]=fixSats, vals[2]=visible if present.
  if (idx >= 3) {
    used = vals[1];
    vis = vals[2];
  } else if (idx >= 2) {
    used = vals[1];
    vis = vals[1];
  }
  mode = m;
  satsUsed = used;
  satsVisible = vis ? vis : used;
  (void)fix;
  return true;
}
// #endregion FUNC_parseCgnssInfoLine

// #region CLASS_GpsClient
GpsClient::GpsClient(ModemChannel& channel, char* scratch, size_t scratchSize)
    : channel_(channel), scratch_(scratch), scratchSize_(scratchSize) {}
// #endregion CLASS_GpsClient

// #region METHOD_GpsClient_fail
void GpsClient::fail(const char* stage) { failedStage_ = stage; }
// #endregion METHOD_GpsClient_fail

// #region METHOD_GpsClient_sendCommand
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

// #region METHOD_GpsClient_ensurePowered
GpsResult GpsClient::ensurePowered() {
  // Check current power
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
  // Antenna bias — critical for etecl25t6a active patch.
  // Try GPIO 1 then 4, matching gps_probe logic.
  {
    const char* antCmds[][2] = {
        {"AT+CGDRT=1,1", "AT+CGSETV=1,1"},
        {"AT+CGDRT=4,1", "AT+CGSETV=4,1"},
    };
    for (auto& pair : antCmds) {
      if (sendCommand(pair[0], 2000) == GpsResult::kSuccess) readResponse();
      if (sendCommand(pair[1], 2000) == GpsResult::kSuccess) {
        GpsResult rr = readResponse();
        if (rr == GpsResult::kSuccess) break;
      }
    }
  }
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
    // Still continue to poll? Mark not powered
    tmp.powered = false;
  } else {
    tmp.powered = true;
  }

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
        snprintf(tmp.lastNmeaLat, sizeof(tmp.lastNmeaLat), "%s", fix.rawLat);
        snprintf(tmp.lastNmeaLon, sizeof(tmp.lastNmeaLon), "%s", fix.rawLon);
        gpsFixToIso(fix, tmp.isoTime, sizeof(tmp.isoTime));
      }
    }
  }
  // CGNSSINFO for sats
  if (sendCommand("AT+CGNSSINFO", 3000) == GpsResult::kSuccess &&
      readResponse() == GpsResult::kSuccess) {
    int mode = -1, used = 0, vis = 0;
    if (parseCgnssInfoLine(scratch_, mode, used, vis)) {
      if (mode >= 0) tmp.mode = mode;
      tmp.satsUsed = used;
      tmp.satsVisible = vis;
    }
  }
  out = tmp;
  return GpsResult::kSuccess;
}
// #endregion METHOD_GpsClient_poll
