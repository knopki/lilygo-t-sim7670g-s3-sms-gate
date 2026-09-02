// #region MODULE_CONTRACT
// PURPOSE: Makes SIM7670G SMS wire behavior testable before hardware I/O.
// SCOPE:
// - Parses SIM7670G replies and drives bounded status and SMS dialogs over ModemChannel.
// - NOT: Serial transport ownership, persisted configuration, task scheduling, and HTTP rendering.
// INVARIANTS:
// - Every exit leaves channel idle;
// - failedStage is stable;
// - parsers tolerate 99/255 unknown sentinels;
// - CMS/CME ERROR shapes are recognised.
// #endregion MODULE_CONTRACT

#include "modem/modem_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include "codec.h"
#include "system/calendar_validate.h"
#include "system/sms_validate.h"

namespace {
constexpr unsigned long kModemDefaultTimeoutMs = 1000;
constexpr unsigned long kModemSendTimeoutMs = 60000;
constexpr int kModemInitRetries = 10;
constexpr int kModemMaxLines = 30;
// CMGL="ALL" of a full SM (30 SMS) needs >=61 meaningful lines (header+body
// pairs and the terminal OK) plus empty framing lines and URCs; the budget is
// set on total read attempts so framing noise cannot exhaust it early.
constexpr int kModemCmglMaxReadAttempts = 128;
}  // namespace

// #region FUNC_isModemNetworkRegistered
// PURPOSE: Accepts home and roaming packet or circuit registration before
// callers trust network-provided data.
bool isModemNetworkRegistered(const ModemStatus& status) {
  return status.ceregStat == 1 || status.ceregStat == 5 || status.cregStat == 1 ||
         status.cregStat == 5;
}
// #endregion FUNC_isModemNetworkRegistered

// #region FUNC_parseCpinLine
// PURPOSE: Parses the +CPIN code so the READY send/forward gate and the
// status UI share one tested parser; unknown codes surface verbatim
// (READY, SIM PIN, NOT INSERTED…).
bool parseCpinLine(const char* line, char* out, size_t outSize) {
  const char* p = strstr(line, "+CPIN:");
  if (p == nullptr) return false;
  p += 6;
  while (*p == ' ' || *p == '\t') ++p;
  size_t len = strlen(p);
  while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n' || p[len - 1] == ' ')) --len;
  if (len + 1 > outSize) return false;
  memcpy(out, p, len);
  out[len] = '\0';
  return true;
}
// #endregion FUNC_parseCpinLine

// #region FUNC_parseCsqLine
// PURPOSE: Parses +CSQ under 27.007 bounds (rssi 0-31/99, ber 0-7/99), so
// signal fields stay sentinel-guarded instead of trusting modem prose.
bool parseCsqLine(const char* line, int& rssi, int& ber) {
  const char* p = strstr(line, "+CSQ:");
  if (p == nullptr) return false;
  if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) != 2) return false;
  return (rssi >= 0 && rssi <= 31) || rssi == 99;
}
// #endregion FUNC_parseCsqLine

// #region FUNC_parseCesqLine
// PURPOSE: Parses the +CESQ <rsrq>,<rsrp> tail and folds the 255 unknown
// sentinel to 0, so the UI never renders a bogus dBm/dB figure.
bool parseCesqLine(const char* line, int& rsrpDbm, int& rsrqDb) {
  // +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
  int rxlev = 0;
  int ber = 0;
  int rscp = 0;
  int ecno = 0;
  int rsrq = 0;
  int rsrp = 0;
  const char* p = strstr(line, "+CESQ:");
  if (p == nullptr) return false;
  if (sscanf(p, "+CESQ: %d,%d,%d,%d,%d,%d", &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) != 6)
    return false;
  if (rsrp == 255)
    rsrpDbm = 0;
  else
    rsrpDbm = -140 + rsrp;
  if (rsrq == 255)
    rsrqDb = 0;
  else
    rsrqDb = -20 + rsrq / 2;
  return true;
}
// #endregion FUNC_parseCesqLine

// #region FUNC_parseCregLine
// PURPOSE: Parses <stat> from both +CREG and +CEREG flavors, so readiness
// checks need one parser regardless of which registration the network
// reports.
bool parseCregLine(const char* line, int& stat) {
  const char* p = strstr(line, "+CREG:");
  if (p == nullptr) p = strstr(line, "+CEREG:");
  if (p == nullptr) return false;
  const char* comma = strchr(p, ',');
  if (comma == nullptr) return false;
  stat = atoi(comma + 1);
  return stat >= 0 && stat <= 8;
}
// #endregion FUNC_parseCregLine

// #region FUNC_parseCopsLine
// PURPOSE: Parses +COPS and accepts the operator-less reply shape, so an
// unregistered modem degrades to a blank operator field instead of a parse
// failure.
bool parseCopsLine(const char* line, char* op, size_t opSize, int& act) {
  const char* p = strstr(line, "+COPS:");
  if (p == nullptr) return false;
  char oper[32] = "";
  int mode = 0;
  int fmt = 0;
  if (sscanf(p, "+COPS: %d,%d,\"%31[^\"]\",%d", &mode, &fmt, oper, &act) == 4) {
    snprintf(op, opSize, "%s", oper);
    return true;
  }
  if (strstr(p, "+COPS: 0") != nullptr || strstr(p, "+COPS:") != nullptr) {
    op[0] = '\0';
    act = -1;
    return true;
  }
  return false;
}
// #endregion FUNC_parseCopsLine

// #region FUNC_parseCpmsLine
// PURPOSE: Parses one store's used/total from +CPMS, so storage counters
// stay best-effort display data that cannot fail a status poll.
bool parseCpmsLine(const char* line, uint16_t& used, uint16_t& total) {
  const char* p = strstr(line, "+CPMS:");
  if (p == nullptr) return false;
  int u = 0;
  int t = 0;
  if (sscanf(p, "+CPMS: \"%*[^\"]\",%d,%d", &u, &t) == 2) {
    used = static_cast<uint16_t>(u);
    total = static_cast<uint16_t>(t);
    return true;
  }
  return false;
}
// #endregion FUNC_parseCpmsLine

// #region FUNC_parseCclkLine
// PURPOSE: Extracts the quoted +CCLK value so the NITZ time feed can
// convert modem time (cclkToEpochMs) without re-parsing the reply line.
bool parseCclkLine(const char* line, char* out, size_t outSize) {
  const char* p = strstr(line, "+CCLK:");
  if (p == nullptr) return false;
  const char* q = strchr(p, '"');
  if (q == nullptr) return false;
  const char* r = strchr(q + 1, '"');
  if (r == nullptr) return false;
  size_t len = static_cast<size_t>(r - (q + 1));
  if (len + 1 > outSize) return false;
  memcpy(out, q + 1, len);
  out[len] = '\0';
  return true;
}
// #endregion FUNC_parseCclkLine

// #region FUNC_cclkToEpochMs
// PURPOSE: Converts modem clock text into UTC for time-source arbitration.
bool cclkToEpochMs(const char* cclk, int64_t& epochMsOut) {
  if (cclk == nullptr) return false;
  // Expected "yy/MM/dd,hh:mm:ss+zz" or "-zz", zz quarters 15 min.
  int yy = 0, mm = 0, dd = 0, hh = 0, mi = 0, ss = 0;
  int tzQuarters = 0;
  char tzSign = '+';
  int scanned =
      sscanf(cclk, "%d/%d/%d,%d:%d:%d%c%d", &yy, &mm, &dd, &hh, &mi, &ss, &tzSign, &tzQuarters);
  if (scanned < 6) return false;
  if (scanned == 6) {
    tzQuarters = 0;
  } else {
    if (tzSign == '-')
      tzQuarters = -tzQuarters;
    else if (tzSign != '+')
      return false;
  }
  const int fullYear = yy + (yy < 70 ? 2000 : 1900);
  if (yy < 0 || yy > 99 || !isValidCalendarDate(fullYear, mm, dd) || hh < 0 || hh > 23 || mi < 0 ||
      mi > 59 || ss < 0 || ss > 59 || tzQuarters < -48 || tzQuarters > 48)
    return false;
  auto daysFromCivil = [](int y, int m, int d) -> int64_t {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + doe - 719468LL;
  };
  int64_t days = daysFromCivil(fullYear, mm, dd);
  int64_t localSec = days * 86400LL + hh * 3600LL + mi * 60LL + ss;
  int64_t tzOffsetSec = (int64_t)tzQuarters * 15 * 60;
  int64_t utcSec = localSec - tzOffsetSec;
  epochMsOut = utcSec * 1000LL;
  return true;
}
// #endregion FUNC_cclkToEpochMs

// #region FUNC_parseImeiLine
// PURPOSE: Extracts the IMEI from both CGSN/GSN reply shapes, so status
// identification does not depend on one firmware's echo style.
bool parseImeiLine(const char* line, char* out, size_t outSize) {
  if (line == nullptr || out == nullptr || outSize == 0) return false;
  const char* p = strchr(line, ':');
  const char* s = (p != nullptr) ? p + 1 : line;
  while (*s == ' ' || *s == '\t' || *s == '"') ++s;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '"' ||
                     s[len - 1] == '\r' || s[len - 1] == '\n'))
    --len;
  if (len < 14 || len >= outSize) return false;
  for (size_t i = 0; i < len; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  memcpy(out, s, len);
  out[len] = '\0';
  return true;
}
// #endregion FUNC_parseImeiLine

// #region FUNC_parseFwLine
// PURPOSE: Extracts the firmware string from +CGMR in either reply shape,
// so diagnostics identify the modem build without hardcoding one format.
bool parseFwLine(const char* line, char* out, size_t outSize) {
  if (line == nullptr || out == nullptr || outSize == 0) return false;
  const char* p = strstr(line, "+CGMR:");
  const char* s = (p != nullptr) ? p + 6 : line;
  while (*s == ' ' || *s == '\t') ++s;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' ||
                     s[len - 1] == '\n' || s[len - 1] == '"'))
    --len;
  if (s[0] == '"' && len > 0 && s[len - 1] == '"') {
    ++s;
    len -= 2;
  }
  if (len == 0 || len + 1 > outSize) return false;
  memcpy(out, s, len);
  out[len] = '\0';
  return true;
}
// #endregion FUNC_parseFwLine

// #region FUNC_decodeModemText
// PURPOSE: Normalizes modem text fields that arrive UCS2-hex
// (CSCS="UCS2") or plain, so callers always receive UTF-8 regardless of
// the charset the modem used for the field.
bool decodeModemText(const char* encoded, char* out, size_t outSize) {
  if (encoded == nullptr || out == nullptr || outSize == 0) return false;
  if (encoded[0] == '\0') {
    out[0] = '\0';
    return true;
  }
  const size_t len = strlen(encoded);
  if (codec::isUcs2HexView(encoded, len)) {
    codec::decodeUcs2HexView(encoded, len, out, outSize);
    return true;
  }
  if (len + 1 > outSize) return false;
  memcpy(out, encoded, len + 1);
  return true;
}
// #endregion FUNC_decodeModemText

// #region FUNC_parseQuotedField
// PURPOSE: Advances a cursor across one bounds-checked quoted field, so
// both header parsers share a single extractor for the quoted-field
// grammar.
static bool parseQuotedField(const char*& cursor, char* out, size_t outSize) {
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (*cursor != '"') return false;
  ++cursor;
  const char* start = cursor;
  const char* end = strchr(start, '"');
  if (end == nullptr) return false;
  const size_t len = static_cast<size_t>(end - start);
  if (len + 1 > outSize) return false;
  memcpy(out, start, len);
  out[len] = '\0';
  cursor = end + 1;
  return true;
}
// #endregion FUNC_parseQuotedField

// #region FUNC_parseCmglHeader
// PURPOSE: Parses +CMGL headers with or without the CSDH tail, so the
// inbox scan works on both firmware reply shapes without a probing
// round-trip.
bool parseCmglHeader(const char* line, ModemCmglInfo& out) {
  out = ModemCmglInfo{};
  if (line == nullptr) return false;
  const char* p = strstr(line, "+CMGL:");
  if (p == nullptr) return false;
  p += 6;
  while (*p == ' ' || *p == '\t') ++p;
  char* endPtr = nullptr;
  long idx = strtol(p, &endPtr, 10);
  if (endPtr == p || idx < 0 || idx > 65535) return false;
  out.idx = static_cast<uint16_t>(idx);
  p = endPtr;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p != ',') return false;
  ++p;
  char statRaw[16] = "";
  if (!parseQuotedField(p, statRaw, sizeof(statRaw))) return false;
  {
    size_t sl = strlen(statRaw);
    if (sl + 1 > sizeof(out.stat)) return false;
    memcpy(out.stat, statRaw, sl + 1);
  }
  if (*p != ',') return false;
  ++p;
  char oaRaw[64] = "";
  if (!parseQuotedField(p, oaRaw, sizeof(oaRaw))) return false;
  if (!decodeModemText(oaRaw, out.oa, sizeof(out.oa))) return false;
  if (*p != ',') return false;
  ++p;
  char emptyRaw[8] = "";
  if (!parseQuotedField(p, emptyRaw, sizeof(emptyRaw))) return false;
  if (*p != ',') return false;
  ++p;
  char sctsRaw[32] = "";
  if (!parseQuotedField(p, sctsRaw, sizeof(sctsRaw))) return false;
  {
    size_t l = strlen(sctsRaw);
    if (l + 1 > sizeof(out.scts)) return false;
    memcpy(out.scts, sctsRaw, l + 1);
  }
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') {
    out.hasTail = false;
    return true;
  }
  if (*p != ',') return false;
  ++p;
  while (*p == ' ' || *p == '\t') ++p;
  long tooa = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.tooa = static_cast<int>(tooa);
  p = endPtr;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p != ',') return false;
  ++p;
  while (*p == ' ' || *p == '\t') ++p;
  long msgLen = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.msgLen = static_cast<int>(msgLen);
  out.hasTail = true;
  return true;
}
// #endregion FUNC_parseCmglHeader

// #region FUNC_parseCmgrHeader
// PURPOSE: Parses +CMGR headers with or without the CSDH tail, so the
// full-body read tolerates both firmware reply shapes.
bool parseCmgrHeader(const char* line, ModemCmgrInfo& out) {
  out = ModemCmgrInfo{};
  if (line == nullptr) return false;
  const char* p = strstr(line, "+CMGR:");
  if (p == nullptr) return false;
  p += 6;
  while (*p == ' ' || *p == '\t') ++p;
  char statRaw[16] = "";
  if (!parseQuotedField(p, statRaw, sizeof(statRaw))) return false;
  memcpy(out.stat, statRaw, sizeof(out.stat));
  if (*p != ',') return false;
  ++p;
  char oaRaw[64] = "";
  if (!parseQuotedField(p, oaRaw, sizeof(oaRaw))) return false;
  if (!decodeModemText(oaRaw, out.oa, sizeof(out.oa))) return false;
  if (*p != ',') return false;
  ++p;
  char emptyRaw[8] = "";
  if (!parseQuotedField(p, emptyRaw, sizeof(emptyRaw))) return false;
  if (*p != ',') return false;
  ++p;
  char sctsRaw[32] = "";
  if (!parseQuotedField(p, sctsRaw, sizeof(sctsRaw))) return false;
  memcpy(out.scts, sctsRaw, sizeof(out.scts));
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') {
    out.hasTail = false;
    return true;
  }
  if (*p != ',') return false;
  ++p;
  char* endPtr = nullptr;
  long tooa = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.tooa = static_cast<int>(tooa);
  p = endPtr;
  if (*p != ',') return false;
  ++p;
  long fo = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.fo = static_cast<int>(fo);
  p = endPtr;
  if (*p != ',') return false;
  ++p;
  long pid = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.pid = static_cast<int>(pid);
  p = endPtr;
  if (*p != ',') return false;
  ++p;
  long dcs = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.dcs = static_cast<int>(dcs);
  p = endPtr;
  if (*p != ',') return false;
  ++p;
  char scaRaw[64] = "";
  if (!parseQuotedField(p, scaRaw, sizeof(scaRaw))) return false;
  if (!decodeModemText(scaRaw, out.sca, sizeof(out.sca))) return false;
  if (*p != ',') return false;
  ++p;
  long tosca = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.tosca = static_cast<int>(tosca);
  p = endPtr;
  if (*p != ',') return false;
  ++p;
  long msgLen = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  out.msgLen = static_cast<int>(msgLen);
  out.hasTail = true;
  return true;
}
// #endregion FUNC_parseCmgrHeader

// #region FUNC_parseCmglEntry
// PURPOSE: Mirrors parseCmgrEntry for CMGL pairs, so listing and
// single-read sources produce ModemSms through one tested decode contract.
bool parseCmglEntry(const char* headerLine, const char* bodyLine, ModemSms& out) {
  out = ModemSms{};
  ModemCmglInfo info{};
  if (!parseCmglHeader(headerLine, info)) return false;
  snprintf(out.id, sizeof(out.id), "%u", static_cast<unsigned>(info.idx));
  {
    size_t l = strlen(info.oa);
    if (l >= sizeof(out.number)) l = sizeof(out.number) - 1;
    memcpy(out.number, info.oa, l);
    out.number[l] = '\0';
  }
  {
    size_t l = strlen(info.scts);
    if (l >= sizeof(out.date)) l = sizeof(out.date) - 1;
    memcpy(out.date, info.scts, l);
    out.date[l] = '\0';
  }
  const char* body = bodyLine != nullptr ? bodyLine : "";
  // Body may be quoted in some firmware variants; strip quotes if present.
  // Max UCS2 bodies: 335 units *4 =1340 hex + quotes, so need >1400.
  char bodyRaw[1536] = "";
  size_t bl = strlen(body);
  if (bl >= 2 && body[0] == '"' && body[bl - 1] == '"') {
    if (bl - 2 >= sizeof(bodyRaw)) return false;
    memcpy(bodyRaw, body + 1, bl - 2);
    bodyRaw[bl - 2] = '\0';
    body = bodyRaw;
  }
  if (body[0] == '\0') {
    out.text[0] = '\0';
  } else if (codec::isUcs2HexView(body, strlen(body)))
    codec::decodeUcs2HexView(body, strlen(body), out.text, sizeof(out.text));
  else {
    size_t tl = strlen(body);
    if (tl >= sizeof(out.text)) tl = sizeof(out.text) - 1;
    memcpy(out.text, body, tl);
    out.text[tl] = '\0';
  }
  out.concatComplete = true;
  snprintf(out.concatReceived, sizeof(out.concatReceived), "1");
  snprintf(out.concatTotal, sizeof(out.concatTotal), "1");
  return true;
}
// #endregion FUNC_parseCmglEntry

// #region FUNC_parseCmgrEntry
// PURPOSE: Builds the delivered ModemSms from the CMGR pair, so forwarding
// gets the complete UCS2-decoded text that CMGL bodies truncate.
bool parseCmgrEntry(const char* headerLine, const char* bodyLine, ModemSms& out) {
  out = ModemSms{};
  ModemCmgrInfo info{};
  if (!parseCmgrHeader(headerLine, info)) return false;
  {
    size_t l = strlen(info.oa);
    if (l >= sizeof(out.number)) l = sizeof(out.number) - 1;
    memcpy(out.number, info.oa, l);
    out.number[l] = '\0';
  }
  {
    size_t l = strlen(info.scts);
    if (l >= sizeof(out.date)) l = sizeof(out.date) - 1;
    memcpy(out.date, info.scts, l);
    out.date[l] = '\0';
  }
  const char* body = bodyLine != nullptr ? bodyLine : "";
  char bodyRaw[1536] = "";
  size_t bl = strlen(body);
  if (bl >= 2 && body[0] == '"' && body[bl - 1] == '"') {
    if (bl - 2 >= sizeof(bodyRaw)) return false;
    memcpy(bodyRaw, body + 1, bl - 2);
    bodyRaw[bl - 2] = '\0';
    body = bodyRaw;
  }
  if (body[0] == '\0')
    out.text[0] = '\0';
  else if (codec::isUcs2HexView(body, strlen(body)))
    codec::decodeUcs2HexView(body, strlen(body), out.text, sizeof(out.text));
  else {
    size_t tl = strlen(body);
    if (tl >= sizeof(out.text)) tl = sizeof(out.text) - 1;
    memcpy(out.text, body, tl);
    out.text[tl] = '\0';
  }
  out.concatComplete = true;
  snprintf(out.concatReceived, sizeof(out.concatReceived), "1");
  snprintf(out.concatTotal, sizeof(out.concatTotal), "1");
  return true;
}
// #endregion FUNC_parseCmgrEntry

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static bool hexByte(const char* p, uint8_t& out) {
  int hi = hexVal(p[0]);
  int lo = hexVal(p[1]);
  if (hi < 0 || lo < 0) return false;
  out = static_cast<uint8_t>((hi << 4) | lo);
  return true;
}

// #region FUNC_parseUdhConcat
// PURPOSE: Walks exactly one bounded UDH IE sequence, so only explicit
// concat IEs contribute metadata and malformed IE boundaries never make a
// message body or unrelated IE look like concatenation state.
static bool parseUdhConcat(const char* udhHex, ModemConcatInfo& out) {
  out = ModemConcatInfo{};
  if (udhHex == nullptr) return false;
  const size_t chars = strlen(udhHex);
  if (chars < 2 || chars % 2 != 0) return false;
  const size_t bytes = chars / 2;
  for (size_t index = 0; index < chars; ++index) {
    if (hexVal(udhHex[index]) < 0) return false;
  }
  uint8_t udhl = 0;
  if (!hexByte(udhHex, udhl) || bytes != static_cast<size_t>(udhl) + 1) return false;
  for (size_t pos = 1; pos < bytes;) {
    uint8_t iei = 0;
    uint8_t iedl = 0;
    if (pos + 2 > bytes || !hexByte(udhHex + pos * 2, iei) ||
        !hexByte(udhHex + (pos + 1) * 2, iedl) || pos + 2 + static_cast<size_t>(iedl) > bytes)
      return false;
    const size_t data = pos + 2;
    ModemConcatInfo candidate{};
    if (iei == 0x00 && iedl == 3) {
      uint8_t ref = 0;
      if (!hexByte(udhHex + data * 2, ref) || !hexByte(udhHex + (data + 1) * 2, candidate.total) ||
          !hexByte(udhHex + (data + 2) * 2, candidate.seq))
        return false;
      candidate.present = true;
      candidate.ref = ref;
    } else if (iei == 0x08 && iedl == 4) {
      uint8_t refHigh = 0;
      uint8_t refLow = 0;
      if (!hexByte(udhHex + data * 2, refHigh) || !hexByte(udhHex + (data + 1) * 2, refLow) ||
          !hexByte(udhHex + (data + 2) * 2, candidate.total) ||
          !hexByte(udhHex + (data + 3) * 2, candidate.seq))
        return false;
      candidate.present = true;
      candidate.ref = static_cast<uint16_t>((static_cast<uint16_t>(refHigh) << 8) | refLow);
      candidate.refIs16Bit = true;
    }
    if (candidate.present) {
      if (candidate.total == 0 || candidate.seq == 0 || candidate.seq > candidate.total ||
          out.present)
        return false;
      out = candidate;
    }
    pos += 2 + static_cast<size_t>(iedl);
  }
  return true;
}
// #endregion FUNC_parseUdhConcat

// #region FUNC_parsePduConcat
// PURPOSE: Exposes strict UDH concat parsing to host tests and callers that
// already isolated TP-UDH bytes, retaining all reference bits and width.
bool parsePduConcat(const char* udhHex, ModemConcatInfo& out) {
  return parseUdhConcat(udhHex, out) && out.present;
}
// #endregion FUNC_parsePduConcat

// #region FUNC_extractConcatFromDeliverPdu
// PURPOSE: Walks an SMS-DELIVER PDU to its real user-data header before
// strictly parsing bounded UDH IEs, so a concat-like byte sequence in sender
// data or message text cannot turn a single SMS into a cached multipart fragment.
bool extractConcatFromDeliverPdu(const char* pduHex, ModemConcatInfo& out) {
  out = ModemConcatInfo{};
  if (pduHex == nullptr) return false;
  const size_t hexLen = strlen(pduHex);
  if (hexLen == 0 || hexLen % 2 != 0) return false;
  for (size_t i = 0; i < hexLen; ++i) {
    if (hexVal(pduHex[i]) < 0) return false;
  }
  const size_t bytes = hexLen / 2;
  auto byteAt = [&](size_t index, uint8_t& value) {
    return index < bytes && hexByte(pduHex + index * 2, value);
  };
  uint8_t scaLen = 0;
  if (!byteAt(0, scaLen) || static_cast<size_t>(scaLen) + 1 >= bytes) return false;
  size_t pos = static_cast<size_t>(scaLen) + 1;
  uint8_t firstOctet = 0;
  if (!byteAt(pos++, firstOctet) || (firstOctet & 0x03) != 0) return false;
  uint8_t oaDigits = 0;
  if (!byteAt(pos++, oaDigits)) return false;
  // OA type plus semi-octet address, then PID, DCS, SCTS(7), and UDL.
  const size_t oaOctets = (static_cast<size_t>(oaDigits) + 1) / 2;
  if (pos + 1 + oaOctets + 1 + 1 + 7 + 1 > bytes) return false;
  ++pos;  // TP-OA type-of-address
  pos += oaOctets;
  pos += 1 + 1 + 7;  // TP-PID, TP-DCS, TP-SCTS
  uint8_t udl = 0;
  if (!byteAt(pos++, udl)) return false;
  if ((firstOctet & 0x40) == 0) return true;  // TP-UDHI absent: single SMS.
  uint8_t udhl = 0;
  // TP-UD is at most 140 octets, including the TP-UDHL octet.
  if (!byteAt(pos, udhl) || udhl == 0 || udhl > 139 || static_cast<size_t>(udhl) + 1 > udl ||
      pos + 1 + static_cast<size_t>(udhl) > bytes)
    return false;
  char udhHex[281] = "";  // UDHL is one octet, maximum UDH is 140 octets.
  const size_t udhChars = (static_cast<size_t>(udhl) + 1) * 2;
  memcpy(udhHex, pduHex + pos * 2, udhChars);
  udhHex[udhChars] = '\0';
  ModemConcatInfo concat;
  if (!parseUdhConcat(udhHex, concat)) return false;
  out = concat;
  return true;
}
// #endregion FUNC_extractConcatFromDeliverPdu

// #region FUNC_modemSmsUtf16Units
// PURPOSE: Counts UTF-16 code units of UTF-8 text for the 335-unit send
// limit. Delegates to the shared sms_validate.h helper so ZTE and
// SIM7670G share one limit and one UTF-8 validity gate.
static size_t modemSmsUtf16Units(const char* utf8) { return smsUtf16Units(utf8); }
// #endregion FUNC_modemSmsUtf16Units

// #region FUNC_isDirectGsmAsciiText
// PURPOSE: Gates the raw GSM send path so only bytes whose ASCII codes are
// identical to GSM 03.38 go unencoded; mismatched ASCII punctuation and
// every non-ASCII byte must take the UCS2 fallback instead of trusting the
// GSM extension table.
bool isDirectGsmAsciiText(const char* text) {
  if (text == nullptr) return false;
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    const bool safe = (ch >= 0x20 && ch <= 0x23) || (ch >= 0x25 && ch <= 0x3F) ||
                      (ch >= 0x41 && ch <= 0x5A) || (ch >= 0x61 && ch <= 0x7A);
    if (!safe) return false;
  }
  return true;
}
// #endregion FUNC_isDirectGsmAsciiText

// #region FUNC_buildUcs2SubmitPdu
// PURPOSE: Renders one concat part as a standards-conformant SMS-SUBMIT PDU
// (TP-UDHI, DCS 8) so a long text reaches the peer as one reassemblable
// message instead of being silently truncated by single-segment text mode;
// pure so the byte-exact layout stays host-tested.
bool buildUcs2SubmitPdu(const char* number, const char* partUcs2Hex, uint8_t ref, uint8_t total,
                        uint8_t seq, char* out, size_t outSize, size_t& pduOctetsOut) {
  pduOctetsOut = 0;
  if (number == nullptr || partUcs2Hex == nullptr || out == nullptr || outSize == 0) return false;
  if (total == 0 || seq == 0 || seq > total) return false;
  const size_t hexLen = strlen(partUcs2Hex);
  if (hexLen == 0 || hexLen % 4 != 0 || hexLen / 4 > kUcs2PduPartUnits) return false;
  const char* digits = number;
  uint8_t tonNpi = 0x81;  // unknown type / unknown numbering plan by default
  if (*digits == '+') {
    tonNpi = 0x91;
    ++digits;
  }
  const size_t numDigits = strlen(digits);
  if (numDigits == 0 || numDigits > 20) return false;
  const size_t contentOctets = hexLen / 2;
  const size_t udOctets = 6 + contentOctets;  // UDHL octet + 5 UDH bytes + content
  if (udOctets > 255) return false;
  const size_t addrOctets = (numDigits + 1) / 2;
  const size_t tpduOctets = 4 + addrOctets + 3 + udOctets;  // hdr + addr + PID/DCS/UDL + UD
  if (outSize < 2 * (tpduOctets + 1) + 1) return false;     // + SCA octet + NUL
  size_t used = 0;
  auto put = [&](uint8_t v) {
    static const char kHex[] = "0123456789ABCDEF";
    out[used++] = kHex[v >> 4];
    out[used++] = kHex[v & 0x0F];
  };
  put(0x00);  // SCA: no service centre address
  put(0x41);  // SMS-SUBMIT, TP-UDHI=1, VPF=00 (no validity period)
  put(0x00);  // TP-MR
  put(static_cast<uint8_t>(numDigits));
  put(tonNpi);
  for (size_t i = 0; i < addrOctets; ++i) {
    // Semi-octet address: low nibble first, F fills the high nibble when
    // the digit count is odd.
    const int lo = hexVal(digits[2 * i]);
    const int hi = (2 * i + 1 < numDigits) ? hexVal(digits[2 * i + 1]) : hexVal('F');
    if (lo < 0 || hi < 0) return false;
    put(static_cast<uint8_t>((hi << 4) | lo));
  }
  put(0x00);                            // TP-PID
  put(0x08);                            // TP-DCS: UCS2
  put(static_cast<uint8_t>(udOctets));  // TP-UDL counts UDHL + UDH + content
  put(0x05);                            // TP-UDHL: one 5-byte concat header
  put(0x00);                            // IEI: concatenated 8-bit reference
  put(0x03);                            // IE data length
  put(ref);
  put(total);
  put(seq);
  for (size_t i = 0; i < hexLen; i += 2) {
    const int hi = hexVal(partUcs2Hex[i]);
    const int lo = hexVal(partUcs2Hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    put(static_cast<uint8_t>((hi << 4) | lo));
  }
  out[used] = '\0';
  pduOctetsOut = tpduOctets;
  return true;
}
// #endregion FUNC_buildUcs2SubmitPdu

// #region METHOD_ModemClient_ModemClient
// PURPOSE: Gives each AT dialog one isolated channel and bounded workspace.
ModemClient::ModemClient(ModemChannel& channel, char* scratch, size_t scratchSize)
    : channel_(channel), scratch_(scratch), scratchSize_(scratchSize) {}
// #endregion METHOD_ModemClient_ModemClient

// #region METHOD_ModemClient_fail
// PURPOSE: Sets the stable stage token for the last failure, so Serial
// events and the UI report a fixed vocabulary instead of raw modem text.
void ModemClient::fail(const char* stage) { failedStage_ = stage; }
// #endregion METHOD_ModemClient_fail

// #region METHOD_ModemClient_sendCommand
// PURPOSE: Purges stale input, then writes one CRLF-terminated command and
// preserves its response timeout, so every dialog step starts from a clean
// line boundary and receives the time its caller permits.
ModemResult ModemClient::sendCommand(const char* cmd, unsigned long timeoutMs) {
  channel_.purge();
  size_t len = strlen(cmd);
  char withCr[96];
  if (len + 2 >= sizeof(withCr)) {
    fail("cmd_too_long");
    return ModemResult::kProtocolError;
  }
  memcpy(withCr, cmd, len);
  withCr[len++] = '\r';
  withCr[len++] = '\n';
  if (!channel_.write(withCr, len)) {
    fail("write_failed");
    return ModemResult::kTimeout;
  }
  responseTimeoutMs_ = timeoutMs;
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_sendCommand

// #region METHOD_ModemClient_readResponse
// PURPOSE: Drains a reply to its terminal OK/ERROR using the timeout of the
// command that started it, and keeps the last payload line in scratch so
// failure diagnostics carry the decisive modem line while success stays quiet.
ModemResult ModemClient::readResponse() {
  if (scratch_ == nullptr || scratchSize_ == 0) {
    fail("no_scratch");
    return ModemResult::kProtocolError;
  }
  scratch_[0] = '\0';
  replyLen_ = 0;
  char line[160];
  for (int i = 0; i < kModemMaxLines; ++i) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") != 0) {
      // Keep the last non-OK line before OK as payload. After ATE0 echo is
      // off, this captures plain IMEI/CGMR as well as + lines. Before ATE0
      // the echoed "AT" is stored first and then overwritten by the real
      // + reply, so the final scratch is the payload.
      snprintf(scratch_, scratchSize_, "%s", line);
      replyLen_ = strlen(scratch_);
    }
    if (strcmp(line, "OK") == 0) return ModemResult::kSuccess;
    if (strstr(line, "ERROR") != nullptr) {
      fail("modem_error");
      return ModemResult::kProtocolError;
    }
  }
  fail("no_ok");
  return ModemResult::kTimeout;
}
// #endregion METHOD_ModemClient_readResponse

// #region METHOD_ModemClient_init
// PURPOSE: Brings the modem to text-mode SMS readiness so one unsupported
// command cannot block the rest: each step is best-effort with its stage
// token, only absent AT fails init (kNotPresent), and the SMS DONE URC
// wait stays bounded (research §1/§8). Sequence: AT -> ATE0 -> ATV1 ->
// CMEE=2 -> CMGF=1 -> CSCS="UCS2" -> CSDH=1 -> CPMS="ME" -> CNMI=2,1.
ModemResult ModemClient::init() {
  for (int attempt = 0; attempt < kModemInitRetries; ++attempt) {
    ModemResult r = sendCommand("AT", kModemDefaultTimeoutMs);
    if (r == ModemResult::kSuccess) r = readResponse();
    if (r == ModemResult::kSuccess) break;
    if (attempt == kModemInitRetries - 1) {
      fail("not_present");
      return ModemResult::kNotPresent;
    }
  }
  if (sendCommand("ATE0", kModemDefaultTimeoutMs) == ModemResult::kSuccess) readResponse();
  if (sendCommand("ATV1", kModemDefaultTimeoutMs) == ModemResult::kSuccess) readResponse();
  if (sendCommand("AT+CMEE=2", kModemDefaultTimeoutMs) == ModemResult::kSuccess) readResponse();
  if (sendCommand("AT+CMGF=1", kModemDefaultTimeoutMs) == ModemResult::kSuccess) {
    if (readResponse() != ModemResult::kSuccess) fail("cmgf");
  } else
    fail("cmgf");
  if (sendCommand("AT+CSCS=\"UCS2\"", kModemDefaultTimeoutMs) == ModemResult::kSuccess) {
    if (readResponse() != ModemResult::kSuccess) fail("cscs");
  } else
    fail("cscs");
  if (sendCommand("AT+CSDH=1", kModemDefaultTimeoutMs) == ModemResult::kSuccess) {
    if (readResponse() != ModemResult::kSuccess) fail("csdh");
  } else
    fail("csdh");
  if (sendCommand("AT+CPMS=\"ME\",\"ME\",\"ME\"", 2000) == ModemResult::kSuccess) {
    if (readResponse() != ModemResult::kSuccess) fail("cpms");
  } else
    fail("cpms");
  if (sendCommand("AT+CNMI=2,1,0,0,0", kModemDefaultTimeoutMs) == ModemResult::kSuccess) {
    if (readResponse() != ModemResult::kSuccess) fail("cnmi");
  } else
    fail("cnmi");
  // Bounded wait for "SMS DONE" URC after power-on (ADR-0004 §1, research §8):
  // modem emits it asynchronously once SIM/phonebook are ready. Poll
  // without failing init if it does not appear within the status window.
  char line[64];
  for (int i = 0; i < kModemInitRetries; ++i) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) continue;
    if (len == 0) continue;
    if (strstr(line, "SMS DONE") != nullptr) break;
    if (strstr(line, "PB DONE") != nullptr) continue;
    // Stray URCs (QCRDY, +CPIN) are ignored; keep waiting for SMS DONE.
  }
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_init

// #region METHOD_ModemClient_pollStatus
// PURPOSE: Sequential status poll; each command isolated so one failure
// does not poison the whole snapshot (unknown fields stay sentinel).
ModemResult ModemClient::pollStatus(ModemStatus& out) {
  ModemStatus tmp;
  tmp.present = false;
  if (sendCommand("AT", kModemDefaultTimeoutMs) != ModemResult::kSuccess) {
    out = tmp;
    fail("at");
    return ModemResult::kNotPresent;
  }
  if (readResponse() != ModemResult::kSuccess) {
    out = tmp;
    fail("at");
    return ModemResult::kNotPresent;
  }
  tmp.present = true;

  // #region BLOCK_collectStatus
  if (sendCommand("AT+CPIN?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    parseCpinLine(scratch_, tmp.cpin, sizeof(tmp.cpin));
  }
  if (sendCommand("AT+CSQ", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    int rssi = 99;
    int ber = 99;
    if (parseCsqLine(scratch_, rssi, ber)) {
      tmp.csqRssi = rssi;
      tmp.csqBer = ber;
      tmp.csqRssiDbm = (rssi == 99 ? 0 : -113 + 2 * rssi);
    }
  }
  if (sendCommand("AT+CESQ", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    int rsrp = 0;
    int rsrq = 0;
    if (parseCesqLine(scratch_, rsrp, rsrq)) {
      tmp.cesqRsrpDbm = rsrp;
      tmp.cesqRsrqDb = rsrq;
    }
  }
  if (sendCommand("AT+CEREG?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    int stat = -1;
    if (parseCregLine(scratch_, stat)) tmp.ceregStat = stat;
  }
  if (sendCommand("AT+CREG?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    int stat = -1;
    if (parseCregLine(scratch_, stat)) tmp.cregStat = stat;
  }
  if (sendCommand("AT+CGATT?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    tmp.cgatt = (strstr(scratch_, ": 1") != nullptr);
  }
  if (sendCommand("AT+COPS?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    int act = -1;
    parseCopsLine(scratch_, tmp.copsOp, sizeof(tmp.copsOp), act);
    tmp.copsAct = act;
  }
  // Read ME counters, then SM counters via a mem1-only CPMS switch, then
  // always restore mem1 to ME. Best-effort: a storage-counter failure never
  // fails the whole status poll.
  if (sendCommand("AT+CPMS?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    uint16_t used = 0;
    uint16_t total = 0;
    if (parseCpmsLine(scratch_, used, total)) {
      tmp.smsUsedMe = used;
      tmp.smsTotalMe = total;
    }
    if (selectReadStorage("SM") == ModemResult::kSuccess) {
      if (sendCommand("AT+CPMS?", 2000) == ModemResult::kSuccess &&
          readResponse() == ModemResult::kSuccess && parseCpmsLine(scratch_, used, total)) {
        tmp.smsUsedSm = used;
        tmp.smsTotalSm = total;
      }
    }
    selectReadStorage("ME");
  }
  if (sendCommand("AT+CCLK?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    parseCclkLine(scratch_, tmp.cclk, sizeof(tmp.cclk));
  }
  if (tmp.imei[0] == '\0') {
    const char* imeiCmds[] = {"AT+CGSN", "AT+GSN"};
    for (const char* cmd : imeiCmds) {
      if (sendCommand(cmd, 2000) != ModemResult::kSuccess) continue;
      if (readResponse() != ModemResult::kSuccess) continue;
      char imei[24] = "";
      if (parseImeiLine(scratch_, imei, sizeof(imei))) {
        snprintf(tmp.imei, sizeof(tmp.imei), "%s", imei);
        break;
      }
      if (strlen(scratch_) >= 14 && strlen(scratch_) < sizeof(tmp.imei)) {
        bool allDigits = true;
        for (size_t i = 0; i < strlen(scratch_); ++i) {
          if (scratch_[i] < '0' || scratch_[i] > '9') {
            allDigits = false;
            break;
          }
        }
        if (allDigits) {
          snprintf(tmp.imei, sizeof(tmp.imei), "%s", scratch_);
          break;
        }
      }
    }
  }
  if (tmp.fw[0] == '\0') {
    if (sendCommand("AT+CGMR", 2000) == ModemResult::kSuccess &&
        readResponse() == ModemResult::kSuccess) {
      char fw[48] = "";
      if (parseFwLine(scratch_, fw, sizeof(fw))) {
        snprintf(tmp.fw, sizeof(tmp.fw), "%s", fw);
      } else {
        snprintf(tmp.fw, sizeof(tmp.fw), "%s", scratch_);
      }
    }
  }
  // #endregion BLOCK_collectStatus

  tmp.updatedMs = 0;
  out = tmp;
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_pollStatus

// #region METHOD_ModemClient_findOldestUnread
// PURPOSE: Bounded scan of AT+CMGL="ALL" picking the oldest incoming index;
// the reply is drained to its terminal OK (otherwise kTimeout with stage
// timeout/cmgl_no_ok) and the full body is fetched via CMGR only after that
// OK, so a truncated or full-storage listing is never mistaken for an empty
// inbox and never bleeds into the next command. CMGL bodies stay undecoded
// (truncated for Latin on this firmware); CMGR is the body source of truth.
ModemResult ModemClient::findOldestUnread(ModemSms& out, bool& found) {
  out = ModemSms{};
  found = false;
  if (scratch_ == nullptr || scratchSize_ == 0) {
    fail("no_scratch");
    return ModemResult::kProtocolError;
  }
  // Use "ALL" so REC READ (already seen but not deleted after SMTP fail) also gets forwarded;
  // filter to incoming only, like ZTE does for tags 0/1. This also prevents SM/ME drift.
  if (sendCommand("AT+CMGL=\"ALL\"", 5000) != ModemResult::kSuccess) return ModemResult::kTimeout;
  char line[1536];
  uint16_t bestIdx = 0xFFFF;
  bool haveBest = false;
  bool terminalOk = false;
  for (int attempt = 0; attempt < kModemCmglMaxReadAttempts; ++attempt) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) {
      terminalOk = true;
      break;
    }
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
        strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kProtocolError;
    }
    if (strncmp(line, "+CMGL:", 6) == 0) {
      ModemCmglInfo curInfo{};
      if (parseCmglHeader(line, curInfo)) {
        // Only incoming — ignore STO SENT/UNSENT/DRAFT (like ZTE tags 2/3/4)
        if (strcmp(curInfo.stat, "REC UNREAD") == 0 || strcmp(curInfo.stat, "REC READ") == 0) {
          if (!haveBest || curInfo.idx < bestIdx) {
            bestIdx = curInfo.idx;
            haveBest = true;
          }
        }
      }
    }
  }
  if (!terminalOk) {
    // Bounded budget exhausted without terminal OK: the listing is incomplete,
    // so it must not count as an empty inbox and CMGR must not start.
    fail("cmgl_no_ok");
    return ModemResult::kTimeout;
  }
  if (!haveBest) {
    found = false;
    return ModemResult::kSuccess;
  }
  // Fetch the full SMS via CMGR — reliable for both Latin and Cyrillic with CSCS="UCS2"
  char idStr[16];
  snprintf(idStr, sizeof(idStr), "%u", (unsigned)bestIdx);
#ifdef ARDUINO
  Serial.printf("event=modem_cmgl_picked idx=%u fetching_via_cmgr\n", (unsigned)bestIdx);
#endif
  ModemResult r = readSms(idStr, out);
  if (r != ModemResult::kSuccess) return r;
  found = true;
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_findOldestUnread

// #region METHOD_ModemClient_findUnreadCandidates
// PURPOSE: Drains CMGL into ascending incoming message identifiers without
// issuing CMGR, so ModemService can skip RAM-cached concat parts and keep
// scanning for their siblings instead of repeatedly selecting the oldest one.
ModemResult ModemClient::findUnreadCandidates(ModemInboxCandidate* out, size_t capacity,
                                              size_t& count) {
  count = 0;
  if (out == nullptr || capacity == 0) {
    fail("cmgl_input");
    return ModemResult::kProtocolError;
  }
  if (sendCommand("AT+CMGL=\"ALL\"", 5000) != ModemResult::kSuccess) return ModemResult::kTimeout;
  char line[1536];
  bool terminalOk = false;
  for (int attempt = 0; attempt < kModemCmglMaxReadAttempts; ++attempt) {
    const int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) {
      terminalOk = true;
      break;
    }
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
        strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kProtocolError;
    }
    if (strncmp(line, "+CMGL:", 6) != 0) continue;
    ModemCmglInfo info{};
    if (!parseCmglHeader(line, info) ||
        (strcmp(info.stat, "REC UNREAD") != 0 && strcmp(info.stat, "REC READ") != 0))
      continue;
    size_t insert = 0;
    while (insert < count && atoi(out[insert].id) < info.idx) ++insert;
    if (insert < count && atoi(out[insert].id) == info.idx) continue;
    if (count < capacity) {
      for (size_t move = count; move > insert; --move) out[move] = out[move - 1];
      ++count;
    } else if (insert < capacity) {
      for (size_t move = capacity - 1; move > insert; --move) out[move] = out[move - 1];
    } else {
      continue;
    }
    snprintf(out[insert].id, sizeof(out[insert].id), "%u", static_cast<unsigned>(info.idx));
  }
  if (!terminalOk) {
    fail("cmgl_no_ok");
    return ModemResult::kTimeout;
  }
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_findUnreadCandidates

// #region METHOD_ModemClient_readSms
// PURPOSE: Fetches one SMS with its complete body via CMGR, because CMGL
// bodies truncate Latin text on this firmware — CMGR is the body source of
// truth for forwarding.
ModemResult ModemClient::readSms(const char* id, ModemSms& out) {
  out = ModemSms{};
  if (id == nullptr || id[0] == '\0') {
    fail("cmgr_input");
    return ModemResult::kProtocolError;
  }
  char* end = nullptr;
  long idx = strtol(id, &end, 10);
  if (end == id || *end != '\0' || idx < 0 || idx > 65535) {
    fail("cmgr_input");
    return ModemResult::kProtocolError;
  }
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "AT+CMGR=%ld", idx);
  if (sendCommand(cmd, 3000) != ModemResult::kSuccess) return ModemResult::kTimeout;
  char line[1536];
  char header[1536] = "";
  bool haveHeader = false;
  char body[1536] = "";
  bool haveBody = false;
  bool terminalOk = false;
  for (int i = 0; i < kModemInitRetries; ++i) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) {
      terminalOk = true;
      break;
    }
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
        strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kProtocolError;
    }
    if (strncmp(line, "+CMGR:", 6) == 0) {
      snprintf(header, sizeof(header), "%s", line);
      haveHeader = true;
      continue;
    }
    if (haveHeader && !haveBody) {
      snprintf(body, sizeof(body), "%s", line);
      haveBody = true;
    }
  }
  if (!terminalOk) {
    fail("cmgr_no_ok");
    return ModemResult::kTimeout;
  }
  if (!haveHeader) {
    fail("cmgr");
    return ModemResult::kProtocolError;
  }
  if (!parseCmgrEntry(header, haveBody ? body : "", out)) {
    fail("cmgr_parse");
    return ModemResult::kProtocolError;
  }
  snprintf(out.id, sizeof(out.id), "%s", id);
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_readSms

// #region METHOD_ModemClient_probeConcat
// PURPOSE: Obtains only SMS-DELIVER UDH metadata in a bounded PDU-mode
// CMGR probe while preserving text mode for body reads, so multipart parts
// can be cached without adding a GSM-7 decoder or changing CMGR text bodies.
ModemResult ModemClient::probeConcat(const char* id, ModemConcatInfo& out) {
  out = ModemConcatInfo{};
  if (id == nullptr || id[0] == '\0') {
    fail("cmgr_input");
    return ModemResult::kProtocolError;
  }
  char* end = nullptr;
  const long idx = strtol(id, &end, 10);
  if (end == id || *end != '\0' || idx < 0 || idx > 65535) {
    fail("cmgr_input");
    return ModemResult::kProtocolError;
  }
  ModemResult result = ModemResult::kSuccess;
  if (sendCommand("AT+CMGF=0", 3000) != ModemResult::kSuccess ||
      readResponse() != ModemResult::kSuccess) {
    fail("cmgf");
    result = ModemResult::kProtocolError;
  } else {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%ld", idx);
    if (sendCommand(cmd, 3000) != ModemResult::kSuccess) {
      result = ModemResult::kTimeout;
    } else {
      char line[1536];
      char pdu[1536] = "";
      bool terminalOk = false;
      for (int attempt = 0; attempt < kModemInitRetries; ++attempt) {
        const int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
        if (len < 0) {
          fail("timeout");
          result = ModemResult::kTimeout;
          break;
        }
        if (len == 0) continue;
        if (strcmp(line, "OK") == 0) {
          terminalOk = true;
          break;
        }
        if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
            strcmp(line, "ERROR") == 0) {
          fail("cms_error");
          result = ModemResult::kProtocolError;
          break;
        }
        if (line[0] != '+' && pdu[0] == '\0') snprintf(pdu, sizeof(pdu), "%s", line);
      }
      if (result == ModemResult::kSuccess &&
          (!terminalOk || pdu[0] == '\0' || !extractConcatFromDeliverPdu(pdu, out))) {
        fail("cmgr_parse");
        result = ModemResult::kProtocolError;
      }
    }
  }
  // Text CMGR/CMGL is the source of bodies. Attempt restoration even after
  // a PDU error; its result never hides the original probe outcome.
  sendCommand("AT+CMGF=1", 3000);
  readResponse();
  return result;
}
// #endregion METHOD_ModemClient_probeConcat

// #region METHOD_ModemClient_deleteSms
// PURPOSE: Erases one SMS only after an explicit modem OK, so the
// at-least-once delivery contract never drops a message that failed to
// delete.
ModemResult ModemClient::deleteSms(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    fail("cmgd_input");
    return ModemResult::kProtocolError;
  }
  char* end = nullptr;
  long idx = strtol(id, &end, 10);
  if (end == id || *end != '\0' || idx < 0 || idx > 65535) {
    fail("cmgd_input");
    return ModemResult::kProtocolError;
  }
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "AT+CMGD=%ld", idx);
  if (sendCommand(cmd, 3000) != ModemResult::kSuccess) return ModemResult::kTimeout;
  char line[64];
  for (int i = 0; i < 5; ++i) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) return ModemResult::kSuccess;
    if (strstr(line, "+CMS ERROR") != nullptr || strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kProtocolError;
    }
  }
  fail("cmgd");
  return ModemResult::kTimeout;
}
// #endregion METHOD_ModemClient_deleteSms
// #region METHOD_ModemClient_selectReadStorage
// PURPOSE: Keeps incoming SMS landing in ME while the inbox scan may work
// on the small SIM store: only the read/delete storage (mem1) switches
// between "ME" and "SM" for subsequent CMGL/CMGR/CMGD; write and incoming
// storages (mem2 and mem3) always stay "ME".
ModemResult ModemClient::selectReadStorage(const char* mem) {
  if (mem == nullptr || (strcmp(mem, "ME") != 0 && strcmp(mem, "SM") != 0)) {
    fail("cpms_input");
    return ModemResult::kProtocolError;
  }
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "AT+CPMS=\"%s\",\"ME\",\"ME\"", mem);
  if (sendCommand(cmd, 2000) != ModemResult::kSuccess) return ModemResult::kTimeout;
  ModemResult r = readResponse();
  if (r != ModemResult::kSuccess) fail("cpms");
  return r;
}
// #endregion METHOD_ModemClient_selectReadStorage

// #region METHOD_ModemClient_restoreUcs2Charset
// PURPOSE: Returns the modem to the UCS2 receive charset after an outgoing
// SMS dialog without replacing that dialog's result or failure stage when
// recovery itself fails.
void ModemClient::restoreUcs2Charset() {
  const char* const previousStage = failedStage_;
  ModemResult result = sendCommand("AT+CSCS=\"UCS2\"", 3000);
  if (result == ModemResult::kSuccess) result = readResponse();
  if (result != ModemResult::kSuccess) {
#ifdef ARDUINO
    Serial.printf("event=modem_charset_restore_failed result=%d\n", static_cast<int>(result));
#endif
  }
  failedStage_ = previousStage;
}
// #endregion METHOD_ModemClient_restoreUcs2Charset

// #region METHOD_ModemClient_submitData
// PURPOSE: Drives the one shared CMGS data dialog — ">" prompt wait,
// payload plus Ctrl-Z, then +CMGS/OK confirmation — so text mode and every
// PDU part reuse one implementation instead of drifting apart.
ModemResult ModemClient::submitData(const char* payload) {
  char line[192] = "";
  bool promptSeen = false;
  for (int attempt = 0; attempt < 6; ++attempt) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      // Timeout waiting for prompt
      fail("cmgs_prompt");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strchr(line, '>') != nullptr) {
      promptSeen = true;
      break;
    }
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
        strcmp(line, "ERROR") == 0) {
      if (scratch_ && scratchSize_ > 0) snprintf(scratch_, scratchSize_, "%s", line);
      fail("cms_error");
      return ModemResult::kSendRejected;
    }
    // Ignore echo and other URCs, keep waiting
  }
  if (!promptSeen) {
    fail("cmgs_prompt");
    return ModemResult::kTimeout;
  }
  if (!channel_.write(payload, strlen(payload))) {
    fail("cms_error");
    return ModemResult::kTimeout;
  }
  const char ctrlZ = 0x1A;
  if (!channel_.write(&ctrlZ, 1)) {
    fail("cms_error");
    return ModemResult::kTimeout;
  }
  bool haveCmgs = false;
  for (int attempt = 0; attempt < 60; ++attempt) {
    int len = channel_.readLine(line, sizeof(line), responseTimeoutMs_);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (scratch_ && scratchSize_ > 0) {
      snprintf(scratch_, scratchSize_, "%s", line);
      replyLen_ = strlen(scratch_);
    }
    if (strncmp(line, "+CMGS:", 6) == 0) {
      haveCmgs = true;
      continue;
    }
    if (strcmp(line, "OK") == 0) {
      if (haveCmgs) return ModemResult::kSuccess;
      fail("protocol");
      return ModemResult::kProtocolError;
    }
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr) {
      fail("cms_error");
      return ModemResult::kSendRejected;
    }
    if (strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kSendRejected;
    }
  }
  fail("timeout");
  return ModemResult::kTimeout;
}
// #endregion METHOD_ModemClient_submitData

// #region METHOD_ModemClient_sendMultipartUcs2
// PURPOSE: Delivers texts beyond one segment as a reassemblable UCS2
// concatenation (SMS-SUBMIT PDU, TP-UDHI) instead of letting text mode
// truncate them in the air; restores text mode and the UCS2 receive charset
// on every outcome because receive/status rely on them.
ModemResult ModemClient::sendMultipartUcs2(const char* number, const char* textUtf8, size_t units) {
  char hexBody[1400] = "";
  // Exact-length check: encodeUcs2Hex truncates silently, which must fail
  // the send instead of delivering a cut message.
  if (codec::encodeUcs2Hex(textUtf8, hexBody, sizeof(hexBody)) != units * 4) {
    fail("send_encode");
    restoreUcs2Charset();
    return ModemResult::kProtocolError;
  }
  uint16_t partStart[kMaxSmsSendMultipartParts];
  uint16_t partLen[kMaxSmsSendMultipartParts];
  size_t partCount = 0;
  for (size_t off = 0; off < units;) {
    if (partCount >= kMaxSmsSendMultipartParts) {
      fail("send_parts");
      restoreUcs2Charset();
      return ModemResult::kProtocolError;
    }
    size_t len = units - off < kUcs2PduPartUnits ? units - off : kUcs2PduPartUnits;
    // A surrogate pair split across parts decodes to garbage at both ends,
    // so shift the boundary back until the pair travels in one part.
    if (off + len < units) {
      const uint32_t lastUnit = codec::parseHex4(hexBody + (off + len - 1) * 4);
      if (lastUnit >= 0xD800 && lastUnit <= 0xDBFF) --len;
    }
    partStart[partCount] = static_cast<uint16_t>(off);
    partLen[partCount] = static_cast<uint16_t>(len);
    ++partCount;
    off += len;
  }
  if (sendCommand("AT+CMGF=0", 3000) != ModemResult::kSuccess) {
    fail("cmgf");
    restoreUcs2Charset();
    return ModemResult::kProtocolError;  // write failed: mode unchanged
  }
  ModemResult result = readResponse();
  if (result != ModemResult::kSuccess) {
    fail("cmgf");
    // Mode state is unknown after a failed switch: put text mode back.
    sendCommand("AT+CMGF=1", 3000);
    readResponse();
    restoreUcs2Charset();
    return result;
  }
#ifdef ARDUINO
  Serial.printf("event=modem_send_diag stage=pdu_mode parts=%u\n",
                static_cast<unsigned>(partCount));
#endif
  const uint8_t ref = static_cast<uint8_t>(++pduRef_);
  for (size_t i = 0; i < partCount && result == ModemResult::kSuccess; ++i) {
    char partHex[4 * kUcs2PduPartUnits + 1];
    memcpy(partHex, hexBody + static_cast<size_t>(partStart[i]) * 4,
           static_cast<size_t>(partLen[i]) * 4);
    partHex[static_cast<size_t>(partLen[i]) * 4] = '\0';
    char pduHex[320];  // 1 SCA + ≤158 TPDU octets → ≤318 hex chars + NUL
    size_t pduOctets = 0;
    if (!buildUcs2SubmitPdu(number, partHex, ref, static_cast<uint8_t>(partCount),
                            static_cast<uint8_t>(i + 1), pduHex, sizeof(pduHex), pduOctets)) {
      fail("send_pdu");
      result = ModemResult::kProtocolError;
      break;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=%u", static_cast<unsigned>(pduOctets));
    channel_.purge();
    if (sendCommand(cmd, 3000) != ModemResult::kSuccess) {
      fail("cmgs_prompt");
      result = ModemResult::kTimeout;
      break;
    }
    result = submitData(pduHex);
  }
  // Receive/status run in text+UCS2 mode: restore it on every outcome; a
  // failed restore surfaces at the next poll, not as a false send result.
  sendCommand("AT+CMGF=1", 3000);
  readResponse();
  restoreUcs2Charset();
  return result;
}
// #endregion METHOD_ModemClient_sendMultipartUcs2

// #region METHOD_ModemClient_sendSms
// PURPOSE: Delivers SMS text undistorted at the peer: single-segment texts
// take the manually proven text mode (raw GSM only for the ASCII subset
// whose codes equal GSM 03.38, UCS2 otherwise); longer texts are handed to
// the UCS2 concat PDU path so the whole shared 335-unit range arrives
// instead of being truncated. Enforces the shared limit and sequences
// CMGF/CSCS/CSMP/CMGS with the ">" prompt, body + 0x1A, then
// +CMGS/+CMS ERROR/OK handling.
ModemResult ModemClient::sendSms(const char* number, const char* textUtf8) {
  if (number == nullptr || textUtf8 == nullptr) {
    fail("send_input");
    return ModemResult::kProtocolError;
  }
  if (!isValidSmsRecipient(number)) {
    fail("send_input");
    return ModemResult::kProtocolError;
  }
  const size_t units = modemSmsUtf16Units(textUtf8);
  if (units == 0 || units == kSmsInvalidUnits || units > kMaxSmsSendUnits) {
    fail("send_input");
    return ModemResult::kProtocolError;
  }
  channel_.purge();
  // Raw GSM only for the ASCII subset whose codes match GSM 03.38; the
  // differing punctuation and any non-ASCII take UCS2 — no extension-table
  // encoder, per the locked plan decisions.
  const bool directGsm = isDirectGsmAsciiText(textUtf8);
  const bool singleSegment = (directGsm && units <= kGsmSingleSegmentUnits) ||
                             (!directGsm && units <= kUcs2TextSegmentUnits);
  char hexNumber[128] = "";
  char hexBody[1400] = "";
  char plainNumber[64] = "";
  const char* numberForAt = nullptr;
  const char* bodyForAt = nullptr;
  const char* cscs = nullptr;
  if (directGsm) {
    cscs = "GSM";
    snprintf(plainNumber, sizeof(plainNumber), "%s", number);
    numberForAt = plainNumber;
    bodyForAt = textUtf8;
  } else {
    cscs = "UCS2";
    // Exact-length check: encodeUcs2Hex truncates silently, which must fail
    // the send instead of delivering a cut message.
    if (codec::encodeUcs2Hex(number, hexNumber, sizeof(hexNumber)) != strlen(number) * 4 ||
        codec::encodeUcs2Hex(textUtf8, hexBody, sizeof(hexBody)) != units * 4) {
      fail("send_encode");
      return ModemResult::kProtocolError;
    }
    numberForAt = hexNumber;
    bodyForAt = hexBody;
  }

  // Step 0: AT sync (wake modem after Serial1 reopen, DTR low)
  sendCommand("AT", 2000);
  readResponse();
  // The AT probe is best-effort: drop any stage it recorded so the send
  // outcome reports its own stage instead of leftover junk.
  failedStage_ = "";
  if (!singleSegment) {
    // A text beyond one segment is silently truncated by text mode
    // (measured on device: 300 chars -> 44 delivered), so hand it to the
    // reassemblable concat path.
    return sendMultipartUcs2(number, textUtf8, units);
  }
  // Step 1: AT+CMGF=1
  if (sendCommand("AT+CMGF=1", 3000) != ModemResult::kSuccess) {
    fail("cmgf");
#ifdef ARDUINO
    Serial.printf("event=modem_send_diag stage=cmgf_send_failed\n");
#endif
    return ModemResult::kProtocolError;
  }
  ModemResult r = readResponse();
  if (r != ModemResult::kSuccess) {
    fail("cmgf");
#ifdef ARDUINO
    Serial.printf("event=modem_send_diag stage=cmgf_reply_failed result=%d\n", (int)r);
#endif
    return r;
  }
  // Step 2: AT+CSCS="<GSM|UCS2>"
  {
    char cscsCmd[32];
    snprintf(cscsCmd, sizeof(cscsCmd), "AT+CSCS=\"%s\"", cscs);
    if (sendCommand(cscsCmd, 3000) != ModemResult::kSuccess) {
      fail("cscs");
      return ModemResult::kProtocolError;
    }
  }
  r = readResponse();
  if (r != ModemResult::kSuccess) {
    fail("cscs");
#ifdef ARDUINO
    Serial.printf("event=modem_send_diag stage=cscs_reply_failed cscs=%s result=%d\n", cscs,
                  (int)r);
#endif
    if (directGsm) restoreUcs2Charset();
    return r;
  }
#ifdef ARDUINO
  Serial.printf("event=modem_send_diag stage=cmgf_cscs_ok cscs=%s body_len=%u\n", cscs,
                (unsigned)strlen(bodyForAt));
#endif
  // Step 2b: AT+CSMP sets DCS (0=GSM 7-bit, 8=UCS2) so peer decodes correctly
  {
    const char* csmp = directGsm ? "AT+CSMP=17,167,0,0" : "AT+CSMP=17,167,0,8";
    if (sendCommand(csmp, 3000) != ModemResult::kSuccess) {
      fail("csmp");
      if (directGsm) restoreUcs2Charset();
      return ModemResult::kProtocolError;
    }
    r = readResponse();
    if (r != ModemResult::kSuccess) {
      fail("csmp");
#ifdef ARDUINO
      Serial.printf("event=modem_send_diag stage=csmp_failed csmp=%s result=%d\n", csmp, (int)r);
#endif
      if (directGsm) restoreUcs2Charset();
      return r;
    }
  }
  // Step 3: AT+CMGS="<number>" -> await ">"
  char cmd[160] = "";
  snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", numberForAt);
  channel_.purge();
  size_t cmdLen = strlen(cmd);
  char withCr[192];
  if (cmdLen + 2 >= sizeof(withCr)) {
    fail("cmgs_prompt");
    if (directGsm) restoreUcs2Charset();
    return ModemResult::kProtocolError;
  }
  memcpy(withCr, cmd, cmdLen);
  withCr[cmdLen++] = '\r';
  withCr[cmdLen++] = '\n';
  if (!channel_.write(withCr, cmdLen)) {
    fail("cmgs_prompt");
    if (directGsm) restoreUcs2Charset();
    return ModemResult::kTimeout;
  }
  r = submitData(bodyForAt);
  if (directGsm) restoreUcs2Charset();
  return r;
}
// #endregion METHOD_ModemClient_sendSms
