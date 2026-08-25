// #region MODULE_CONTRACT
// PURPOSE: Implements the host-testable SIM7670G AT dialog (ADR-0004).
// INVARIANTS: Every exit leaves channel idle; failedStage is stable;
// parsers tolerate 99/255 unknown sentinels; CMS/CME ERROR shapes are
// recognised; all non-trivial functions have GRACE contracts.
// #endregion MODULE_CONTRACT

#include "modem_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  for (int i = 0; i < 30; ++i) {
    int len = channel_.readLine(line, sizeof(line), 1000);
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
// PURPOSE: Bring-up sequence: AT -> ATE0 -> CMEE=2 -> wait SMS DONE.
ModemResult ModemClient::init() {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (sendCommand("AT", 1000) != ModemResult::kSuccess) continue;
    ModemResult r = readResponse();
    if (r == ModemResult::kSuccess) break;
    if (attempt == 9) {
      fail("not_present");
      return ModemResult::kNotPresent;
    }
  }
  if (sendCommand("ATE0", 1000) == ModemResult::kSuccess) readResponse();
  if (sendCommand("AT+CMEE=2", 1000) == ModemResult::kSuccess) readResponse();
  return ModemResult::kSuccess;
}
// #endregion METHOD_ModemClient_init

// #region METHOD_ModemClient_pollStatus
// PURPOSE: Sequential status poll; each command isolated so one failure
// does not poison the whole snapshot (unknown fields stay sentinel).
ModemResult ModemClient::pollStatus(ModemStatus& out) {
  ModemStatus tmp;
  tmp.present = false;
  if (sendCommand("AT", 1000) != ModemResult::kSuccess) {
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
ModemResult ModemClient::findOldestUnread(ModemSms& out, bool& found) {
  out = ModemSms{};
  found = false;
  fail("not_implemented");
  return ModemResult::kProtocolError;
}
// #endregion METHOD_ModemClient_findOldestUnread
// #region METHOD_ModemClient_readSms
ModemResult ModemClient::readSms(const char* id, ModemSms& out) {
  (void)id;
  out = ModemSms{};
  fail("not_implemented");
  return ModemResult::kProtocolError;
}
// #endregion METHOD_ModemClient_readSms
// #region METHOD_ModemClient_deleteSms
ModemResult ModemClient::deleteSms(const char* id) {
  (void)id;
  fail("not_implemented");
  return ModemResult::kProtocolError;
}
// #endregion METHOD_ModemClient_deleteSms
// #region METHOD_ModemClient_sendSms
ModemResult ModemClient::sendSms(const char* number, const char* textUtf8) {
  (void)number;
  (void)textUtf8;
  fail("not_implemented");
  return ModemResult::kProtocolError;
}
// #endregion METHOD_ModemClient_sendSms
