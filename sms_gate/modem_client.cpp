// #region MODULE_CONTRACT
// PURPOSE: Implements the host-testable SIM7670G AT dialog (ADR-0004).
// INVARIANTS: Every exit leaves channel idle; failedStage is stable;
// parsers tolerate 99/255 unknown sentinels; CMS/CME ERROR shapes are
// recognised; all non-trivial functions have GRACE contracts.
// #endregion MODULE_CONTRACT

#include "modem_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "sms_validate.h"

namespace {
constexpr unsigned long kModemDefaultTimeoutMs = 1000;
constexpr unsigned long kModemSendTimeoutMs = 60000;
constexpr int kModemInitRetries = 10;
constexpr int kModemMaxLines = 30;
}  // namespace

// #region FUNC_parseCpinLine
// PURPOSE: Parses +CPIN: <code> line into out (READY, SIM PIN, NOT INSERTED…).
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
// PURPOSE: Parses +CSQ: <rssi>,<ber> (27.007). rssi 0-31/99, ber 0-7/99.
bool parseCsqLine(const char* line, int& rssi, int& ber) {
  const char* p = strstr(line, "+CSQ:");
  if (p == nullptr) return false;
  if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) != 2) return false;
  return (rssi >= 0 && rssi <= 31) || rssi == 99;
}
// #endregion FUNC_parseCsqLine

// #region FUNC_parseCesqLine
// PURPOSE: Parses +CESQ: …,<rsrq>,<rsrp> subset; converts to dBm/dB.
// Stores sentinel 255 as 0 dBm/dB for ModemStatus (unknown).
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
// PURPOSE: Parses +CREG/+CEREG: <n>,<stat>[,…] and returns stat.
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
// PURPOSE: Parses +COPS: <mode>,<format>,"<oper>",<act>.
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
// PURPOSE: Parses +CPMS: "ME",<used>,<total> … for one store.
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
// PURPOSE: Parses +CCLK: "yy/MM/dd,hh:mm:ss+zz".
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

// #region FUNC_parseImeiLine
// PURPOSE: Extracts IMEI (14-16 digits) from AT+CGSN/AT+GSN reply, tolerating
// "+CGSN: 864..." prefix or plain "864...".
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
// PURPOSE: Extracts firmware string from AT+CGMR reply, stripping "+CGMR: "
// when present, otherwise returns trimmed plain line.
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
// PURPOSE: Decodes one modem text field that may be UCS2-hex or plain GSM; empty stays empty.
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
// PURPOSE: Extracts one quoted string field advancing cursor past closing quote.
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
// PURPOSE: Parses +CMGL header tolerant to CSDH=0 (no tail) and CSDH=1 (tooa,len).
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
// PURPOSE: Parses +CMGR header tolerant to CSDH=0 and CSDH=1 with 7-field tail.
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
// PURPOSE: Builds one ModemSms from a CMGL header+body pair (CSDH tolerant).
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
  char bodyRaw[1024] = "";
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
// PURPOSE: Builds one ModemSms from a CMGR header+body pair.
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
  char bodyRaw[1024] = "";
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

// #region FUNC_parsePduConcat
// PURPOSE: Scans a PDU hex string for 8-bit concat IE (050003) and returns ref/total/seq.
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
bool parsePduConcat(const char* pduHex, uint8_t& ref, uint8_t& total, uint8_t& seq) {
  if (pduHex == nullptr) return false;
  const size_t len = strlen(pduHex);
  // Search case-insensitive for 050003 (UDHL 05, IEI 00, IEDL 03).
  for (size_t i = 0; i + 12 <= len; ++i) {
    if (tolower((unsigned char)pduHex[i]) != '0' || tolower((unsigned char)pduHex[i + 1]) != '5')
      continue;
    if (tolower((unsigned char)pduHex[i + 2]) != '0' ||
        tolower((unsigned char)pduHex[i + 3]) != '0')
      continue;
    if (tolower((unsigned char)pduHex[i + 4]) != '0' ||
        tolower((unsigned char)pduHex[i + 5]) != '3')
      continue;
    uint8_t r = 0, t = 0, s = 0;
    if (!hexByte(pduHex + i + 6, r)) continue;
    if (!hexByte(pduHex + i + 8, t)) continue;
    if (!hexByte(pduHex + i + 10, s)) continue;
    if (t == 0 || s == 0 || s > t) continue;
    ref = r;
    total = t;
    seq = s;
    return true;
  }
  // 16-bit ref variant: 060804 (UDHL 06, IEI 08, IEDL 04, 2-byte ref).
  for (size_t i = 0; i + 14 <= len; ++i) {
    if (tolower((unsigned char)pduHex[i]) != '0' || tolower((unsigned char)pduHex[i + 1]) != '6')
      continue;
    if (tolower((unsigned char)pduHex[i + 2]) != '0' ||
        tolower((unsigned char)pduHex[i + 3]) != '8')
      continue;
    if (tolower((unsigned char)pduHex[i + 4]) != '0' ||
        tolower((unsigned char)pduHex[i + 5]) != '4')
      continue;
    uint8_t t = 0, s = 0;
    if (!hexByte(pduHex + i + 10, t)) continue;
    if (!hexByte(pduHex + i + 12, s)) continue;
    if (t == 0 || s == 0 || s > t) continue;
    uint8_t rHi = 0, rLo = 0;
    if (!hexByte(pduHex + i + 6, rHi)) continue;
    if (!hexByte(pduHex + i + 8, rLo)) continue;
    ref = rHi;
    total = t;
    seq = s;
    return true;
  }
  return false;
}
// #endregion FUNC_parsePduConcat

// #region FUNC_modemSmsUtf16Units
// PURPOSE: Counts UTF-16 code units of UTF-8 text for the 335-unit send
// limit. Delegates to the shared sms_validate.h helper so ZTE and
// SIM7670G share one limit and one UTF-8 validity gate.
static size_t modemSmsUtf16Units(const char* utf8) { return smsUtf16Units(utf8); }
// #endregion FUNC_modemSmsUtf16Units

// #region CLASS_ModemClient
ModemClient::ModemClient(ModemChannel& channel, char* scratch, size_t scratchSize)
    : channel_(channel), scratch_(scratch), scratchSize_(scratchSize) {}
// #endregion CLASS_ModemClient

// #region METHOD_ModemClient_fail
// PURPOSE: Records stable stage token for Serial/UI.
void ModemClient::fail(const char* stage) { failedStage_ = stage; }
// #endregion METHOD_ModemClient_fail

// #region METHOD_ModemClient_sendCommand
// PURPOSE: Sends one AT command (with CRLF) and drains echo.
ModemResult ModemClient::sendCommand(const char* cmd, unsigned long timeoutMs) {
  (void)timeoutMs;
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
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_sendCommand

// #region METHOD_ModemClient_readResponse
// PURPOSE: Reads lines until OK/ERROR/+CMS ERROR/+CME ERROR; stores last
// non-empty + line in scratch for failedStage diagnostics.
ModemResult ModemClient::readResponse() {
  if (scratch_ == nullptr || scratchSize_ == 0) {
    fail("no_scratch");
    return ModemResult::kProtocolError;
  }
  scratch_[0] = '\0';
  replyLen_ = 0;
  char line[160];
  for (int i = 0; i < kModemMaxLines; ++i) {
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
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
// PURPOSE: Bring-up sequence: AT -> ATE0 -> ATV1 -> CMEE=2 -> CMGF=1 ->
// CSCS="UCS2" -> CSDH=1 -> CPMS="ME" -> CNMI=2,1 + bounded wait for
// "SMS DONE" URC (research §1/§8). Each step is best-effort but reported
// via failedStage on hard AT absence.
ModemResult ModemClient::init() {
  for (int attempt = 0; attempt < kModemInitRetries; ++attempt) {
    if (sendCommand("AT", kModemDefaultTimeoutMs) != ModemResult::kSuccess) continue;
    ModemResult r = readResponse();
    if (r == ModemResult::kSuccess) break;
    if (attempt == 9) {
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
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
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
  if (sendCommand("AT+CPMS?", 2000) == ModemResult::kSuccess &&
      readResponse() == ModemResult::kSuccess) {
    uint16_t used = 0;
    uint16_t total = 0;
    if (parseCpmsLine(scratch_, used, total)) {
      tmp.smsUsedMe = used;
      tmp.smsTotalMe = total;
    }
    if (sendCommand("AT+CPMS=\"SM\",\"SM\",\"SM\"", 2000) == ModemResult::kSuccess) readResponse();
    if (sendCommand("AT+CPMS?", 2000) == ModemResult::kSuccess &&
        readResponse() == ModemResult::kSuccess) {
      if (parseCpmsLine(scratch_, used, total)) {
        tmp.smsUsedSm = used;
        tmp.smsTotalSm = total;
      }
    }
    sendCommand("AT+CPMS=\"ME\",\"ME\",\"ME\"", 2000);
    readResponse();
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
// PURPOSE: Bounded poll of "REC UNREAD" via AT+CMGL, decoding UCS2 and picking oldest by idx.
ModemResult ModemClient::findOldestUnread(ModemSms& out, bool& found) {
  out = ModemSms{};
  found = false;
  if (scratch_ == nullptr || scratchSize_ == 0) {
    fail("no_scratch");
    return ModemResult::kProtocolError;
  }
  if (sendCommand("AT+CMGL=\"REC UNREAD\"", 5000) != ModemResult::kSuccess)
    return ModemResult::kTimeout;
  char line[256];
  char pendingHeader[256] = "";
  bool hasPending = false;
  ModemSms best{};
  uint16_t bestIdx = 0xFFFF;
  bool haveBest = false;
  for (int i = 0; i < 60; ++i) {
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) break;
    if (strstr(line, "+CMS ERROR") != nullptr || strstr(line, "+CME ERROR") != nullptr ||
        strcmp(line, "ERROR") == 0) {
      fail("cms_error");
      return ModemResult::kProtocolError;
    }
    if (strncmp(line, "+CMGL:", 6) == 0) {
      if (hasPending) {
        ModemSms cur{};
        if (parseCmglEntry(pendingHeader, "", cur)) {
          uint16_t idx = static_cast<uint16_t>(atoi(cur.id));
          if (!haveBest || idx < bestIdx) {
            best = cur;
            bestIdx = idx;
            haveBest = true;
          }
        }
      }
      snprintf(pendingHeader, sizeof(pendingHeader), "%s", line);
      hasPending = true;
      continue;
    }
    if (hasPending) {
      ModemSms cur{};
      if (parseCmglEntry(pendingHeader, line, cur)) {
        uint16_t idx = static_cast<uint16_t>(atoi(cur.id));
        if (!haveBest || idx < bestIdx) {
          best = cur;
          bestIdx = idx;
          haveBest = true;
        }
      } else {
        fail("cmgl_parse");
        return ModemResult::kProtocolError;
      }
      hasPending = false;
    }
  }
  if (hasPending) {
    ModemSms cur{};
    if (parseCmglEntry(pendingHeader, "", cur)) {
      uint16_t idx = static_cast<uint16_t>(atoi(cur.id));
      if (!haveBest || idx < bestIdx) {
        best = cur;
        bestIdx = idx;
        haveBest = true;
      }
    }
  }
  if (!haveBest) {
    found = false;
    return ModemResult::kSuccess;
  }
  out = best;
  found = true;
  (void)bestIdx;
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_findOldestUnread
// #region METHOD_ModemClient_readSms
// PURPOSE: Reads one SMS by index via AT+CMGR.
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
  char line[256];
  char header[256] = "";
  bool haveHeader = false;
  char body[1024] = "";
  bool haveBody = false;
  for (int i = 0; i < kModemInitRetries; ++i) {
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
    if (len < 0) {
      fail("timeout");
      return ModemResult::kTimeout;
    }
    if (len == 0) continue;
    if (strcmp(line, "OK") == 0) break;
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
// #region METHOD_ModemClient_deleteSms
// PURPOSE: Deletes one SMS by index via AT+CMGD and expects OK.
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
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
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
// #region METHOD_ModemClient_sendSms
// PURPOSE: Validates recipient and UTF-8 text under the shared 335-unit
// limit and performs the UCS2 AT send: CMGF=1, CSCS="UCS2", CMGS with
// ">" prompt, body + 0x1A, then +CMGS/+CMS ERROR/OK handling.
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
  lastSendHex_[0] = '\0';
  channel_.purge();
  // Encode number and body as UCS2 hex (DRY via codec::encodeUcs2Hex).
  char hexNumber[128] = "";
  char hexBody[1400] = "";
  codec::encodeUcs2Hex(number, hexNumber, sizeof(hexNumber));
  codec::encodeUcs2Hex(textUtf8, hexBody, sizeof(hexBody));
  snprintf(lastSendHex_, sizeof(lastSendHex_), "%s:%s", hexNumber, hexBody);

  // Step 1: AT+CMGF=1
  if (sendCommand("AT+CMGF=1", 3000) != ModemResult::kSuccess) {
    fail("cmgf");
    return ModemResult::kProtocolError;
  }
  ModemResult r = readResponse();
  if (r != ModemResult::kSuccess) {
    fail("cmgf");
    return r;
  }
  // Step 2: AT+CSCS="UCS2"
  if (sendCommand("AT+CSCS=\"UCS2\"", 3000) != ModemResult::kSuccess) {
    fail("cscs");
    return ModemResult::kProtocolError;
  }
  r = readResponse();
  if (r != ModemResult::kSuccess) {
    fail("cscs");
    return r;
  }
  // Step 3: AT+CMGS="<hexNumber>" -> await ">"
  char cmd[160] = "";
  snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", hexNumber);
  channel_.purge();
  size_t cmdLen = strlen(cmd);
  char withCr[192];
  if (cmdLen + 2 >= sizeof(withCr)) {
    fail("cmgs_prompt");
    return ModemResult::kProtocolError;
  }
  memcpy(withCr, cmd, cmdLen);
  withCr[cmdLen++] = '\r';
  withCr[cmdLen++] = '\n';
  if (!channel_.write(withCr, cmdLen)) {
    fail("cmgs_prompt");
    return ModemResult::kTimeout;
  }
  char line[192] = "";
  bool promptSeen = false;
  for (int attempt = 0; attempt < 6; ++attempt) {
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
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
  // Step 4: send body + Ctrl-Z
  if (!channel_.write(hexBody, strlen(hexBody))) {
    fail("cms_error");
    return ModemResult::kTimeout;
  }
  const char ctrlZ = 0x1A;
  if (!channel_.write(&ctrlZ, 1)) {
    fail("cms_error");
    return ModemResult::kTimeout;
  }
  // Step 5: wait for +CMGS: and OK (30-60s)
  bool haveCmgs = false;
  for (int attempt = 0; attempt < 60; ++attempt) {
    int len = channel_.readLine(line, sizeof(line), kModemDefaultTimeoutMs);
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
// #endregion METHOD_ModemClient_sendSms
