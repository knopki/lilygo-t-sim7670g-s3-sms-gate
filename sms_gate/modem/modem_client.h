// #region MODULE_CONTRACT
// PURPOSE: Host-testable AT dialog for the onboard SIM7670G modem (see
// ADR-0004 and docs/research/modem-sim7670g.md). Owns status polling and
// the SMS lifecycle (receive/delete over AT+CMGL/CMGR/CMGD and send over
// AT+CMGS/CSCS/CMGF), over an abstract ModemChannel so logic stays testable
// without hardware.
// SCOPE:
// - Status init and poll (AT, ATE0, CMEE, CPIN, CSQ, CESQ, CEREG, CREG,
//   CGATT, COPS, CPMS, CCLK, CGMR/GSN), line parsing, stable ModemResult and
//   failedStage, ModemSms lifecycle (CMGL/CMGR/CMGD/CMGS/CNMI) including
//   UCS2-hex send (AT+CMGS with ">" prompt and +CMGS/+CMS ERROR handling).
// - NOT: HardwareSerial ownership and pin control (modem_transport.h),
//   SMTP delivery, HTTP routes, NVS persistence.
// INVARIANTS: Every public method leaves the channel in a stopped/idle state
// on return; credentials never appear in stage names; SMS are deleted only
// after SMTP acceptance; parsing tolerates 99/255 unknown sentinels and
// +CMS/+CME ERROR; all public entities have GRACE contracts.
// DEPENDENCIES: Pure C++ (codec.h for UCS2 encode/decode, sms_validate.h for
// 335-unit limit); device channel lives in modem_transport.h; tests use
// FakeModemChannel.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_CLIENT_H
#define MODEM_MODEM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

// #region ENUM_ModemResult
// PURPOSE: Stable outcome per failure class for Serial events and HTTP.
// Values map to UI tokens and log stages like ZteResult/SmtpSendResult.
enum class ModemResult {
  kSuccess,
  kNotPresent,     // No AT OK within timeout
  kTimeout,        // Command timed out
  kProtocolError,  // Unexpected reply shape
  kSimNotReady,    // CPIN != READY
  kSendRejected,   // +CMS ERROR on CMGS
};
// #endregion ENUM_ModemResult

// #region STRUCT_ModemStatus
// PURPOSE: Snapshot of the modem's connectivity and storage state for the
// portMUX cache and GET /api/modem/status. All strings are NUL-terminated
// bounded buffers; RSSI/RSRP use dBm, unknown → 99/255 sentinel handling.
struct ModemStatus {
  bool present = false;  // AT OK seen
  char cpin[32] = "";    // e.g. READY, SIM PIN, NOT INSERTED
  int csqRssi = 99;      // 0-31, 99 unknown
  int csqBer = 99;       // 0-7, 99 unknown
  int csqRssiDbm = 0;    // -113+2*rssi, 0 when unknown
  int cesqRsrpDbm = 0;   // -140+rsrp, 0 when 255
  int cesqRsrqDb = 0;    // -20+0.5*rsrq, 0 when 255
  int cregStat = -1;     // -1 unknown, 0-5 as per 27.007
  int ceregStat = -1;
  bool cgatt = false;    // PS attached
  char copsOp[32] = "";  // operator numeric or long name
  int copsAct = -1;      // 0-7, 7 = E-UTRAN
  char cclk[32] = "";    // raw +CCLK or empty when unsynced
  uint16_t smsUsedMe = 0;
  uint16_t smsTotalMe = 0;
  uint16_t smsUsedSm = 0;
  uint16_t smsTotalSm = 0;
  char imei[24] = "";
  char fw[48] = "";        // CGMR
  uint32_t updatedMs = 0;  // millis() of last successful poll
};
// #endregion STRUCT_ModemStatus

// #region STRUCT_ModemSms
// PURPOSE: One SMS for the future receive/send/delete lifecycle (ADR-0004).
// Kept minimal; mirrors ZteSms shape so buildSmsEmail can be reused.
struct ModemSms {
  char id[16] = "";  // storage index as string
  char number[32] = "";
  char date[32] = "";    // raw +CMGR date field
  char text[1024] = "";  // UTF-8 decoded from UCS2 hex
  bool concatComplete = true;
  char concatReceived[8] = "";
  char concatTotal[8] = "";
};
// #endregion STRUCT_ModemSms

// #region CLASS_ModemChannel
// PURPOSE: Abstracts the AT byte transport so tests can script a modem and
// the device can bind HardwareSerial (Serial1). Mirrors ZteChannel style.
class ModemChannel {
 public:
  virtual ~ModemChannel() = default;
  // Sends exactly len bytes; returns false on write failure.
  virtual bool write(const char* data, size_t len) = 0;
  // Reads one CRLF-terminated line (without CRLF) with timeout.
  // Returns line length, 0 for empty line, <0 on timeout/error.
  virtual int readLine(char* buffer, size_t size, unsigned long timeoutMs) = 0;
  // Purges pending input before next command.
  virtual void purge() = 0;
};
// #endregion CLASS_ModemChannel

// #region CLASS_ModemClient
// PURPOSE: Sequences the AT dialog (init, pollStatus, and future SMS ops)
// and exposes failedStage/lastReply for observable Serial contracts.
class ModemClient {
 public:
  ModemClient(ModemChannel& channel, char* scratch, size_t scratchSize);

  // Bring-up: AT → ATE0 → ATV1 → AT+CMEE=2 → wait SMS DONE (bounded).
  ModemResult init();

  // Polls all status commands into out; present=false when AT not seen.
  ModemResult pollStatus(ModemStatus& out);

  // Future SMS lifecycle — stubs in step 1, real in steps 2-3.
  ModemResult findOldestUnread(ModemSms& out, bool& found);
  ModemResult readSms(const char* id, ModemSms& out);
  ModemResult deleteSms(const char* id);
  ModemResult sendSms(const char* number, const char* textUtf8);

  const char* failedStage() const { return failedStage_; }
  const char* lastReply() const { return scratch_ ? scratch_ : ""; }
  const char* lastSendHex() const { return lastSendHex_; }

 private:
  ModemResult sendCommand(const char* cmd, unsigned long timeoutMs);
  ModemResult readResponse();
  void fail(const char* stage);

  ModemChannel& channel_;
  char* scratch_;
  size_t scratchSize_;
  const char* failedStage_ = "";
  size_t replyLen_ = 0;
  char lastSendHex_[1792] = "";
};
// #endregion CLASS_ModemClient

// Pure parsers exposed for host tests (like zte_client helpers).
bool parseCpinLine(const char* line, char* out, size_t outSize);
bool parseCsqLine(const char* line, int& rssi, int& ber);
bool parseCesqLine(const char* line, int& rsrpDbm, int& rsrqDb);
bool parseCregLine(const char* line, int& stat);
bool parseCopsLine(const char* line, char* op, size_t opSize, int& act);
bool parseCpmsLine(const char* line, uint16_t& used, uint16_t& total);
bool parseCclkLine(const char* line, char* out, size_t outSize);
// Parses +CCLK raw value "yy/MM/dd,hh:mm:ss+zz" to UTC epoch ms (tz quarters → UTC).
bool cclkToEpochMs(const char* cclk, int64_t& epochMsOut);
bool parseImeiLine(const char* line, char* out, size_t outSize);
bool parseFwLine(const char* line, char* out, size_t outSize);
struct ModemCmglInfo {
  uint16_t idx = 0;
  char stat[16] = "";
  char oa[64] = "";  // decoded UTF-8 (from UCS2 hex when needed)
  char scts[32] = "";
  bool hasTail = false;
  int tooa = -1;
  int msgLen = -1;
};
struct ModemCmgrInfo {
  char stat[16] = "";
  char oa[64] = "";
  char scts[32] = "";
  bool hasTail = false;
  int tooa = -1;
  int fo = -1;
  int pid = -1;
  int dcs = -1;
  char sca[64] = "";
  int tosca = -1;
  int msgLen = -1;
};
bool parseCmglHeader(const char* line, ModemCmglInfo& out);
bool parseCmgrHeader(const char* line, ModemCmgrInfo& out);
bool parseCmglEntry(const char* headerLine, const char* bodyLine, ModemSms& out);
bool parseCmgrEntry(const char* headerLine, const char* bodyLine, ModemSms& out);
bool decodeModemText(const char* encoded, char* out, size_t outSize);
bool parsePduConcat(const char* pduHex, uint8_t& ref, uint8_t& total, uint8_t& seq);
#endif  // MODEM_MODEM_CLIENT_H
