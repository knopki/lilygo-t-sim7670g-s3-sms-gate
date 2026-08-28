// #region MODULE_CONTRACT
// PURPOSE: Host-testable AT dialog for the onboard SIM7670G modem (see
// ADR-0004 and docs/research/modem-sim7670g.md). Owns status polling and
// the SMS lifecycle (receive/delete over AT+CMGL/CMGR/CMGD and send over
// AT+CMGS/CSCS/CMGF), over an abstract ModemChannel so logic stays testable
// without hardware.
// SCOPE:
// - Status init and poll (AT, ATE0, CMEE, CPIN, CSQ, CESQ, CEREG, CREG,
//   CGATT, COPS, CPMS, CCLK, CGMR/GSN), line parsing, stable ModemResult and
//   failedStage, ModemSms lifecycle (CMGL/CMGR/CMGD/CMGS/CNMI): bounded
//   CMGL="ALL" scan that drains to terminal OK before CMGR, candidate-list
//   scans for cached concat siblings, full text body via CMGR plus a
//   CMGF=0/CMGR UDH probe that restores CMGF=1 on every outcome, and send
//   (AT+CMGS with ">" prompt, +CMGS/+CMS ERROR handling): single-segment
//   texts take raw GSM only for the GSM-03.38-identical
//   ASCII subset and UCS2-hex otherwise, while longer texts go out as UCS2
//   concat SMS-SUBMIT PDU parts (CMGF=0, TP-UDHI, ≤6×67 units) with text
//   mode restored on every outcome. Inbound UDH parsing retains its full
//   8-/16-bit reference width for cache identity.
// - NOT: HardwareSerial ownership and pin control (modem_transport.h),
//   SMTP delivery, HTTP routes, NVS persistence.
// INVARIANTS: Every public method leaves the channel in a stopped/idle state
// on return; credentials never appear in stage names; SMS are deleted only
// after SMTP acceptance; storage switching touches only mem1 (mem2/mem3
// stay "ME" so incoming messages keep landing there); PDU probes and a
// multipart send restore text mode (CMGF=1) on every outcome; parsing tolerates 99/255
// unknown sentinels and +CMS/+CME ERROR; all public entities have GRACE
// contracts.
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
// PURPOSE: One stable outcome vocabulary for Serial events and the UI
// (like ZteResult/SmtpSendResult), so failures classify into fixed tokens
// instead of quoting modem replies.
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
// PURPOSE: Moves the connectivity/storage snapshot from the poll task to
// the JSON API through the portMUX cache, so HTTP reads never touch
// Serial1. Strings are NUL-terminated bounded buffers; RSSI/RSRP use dBm,
// unknown → 99/255 sentinels.
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
// PURPOSE: One SMS in the forward pipeline; mirrors the ZteSms shape so
// buildSmsEmail stays shared and both sources forward identically.
struct ModemSms {
  char id[16] = "";  // storage index as string
  char number[32] = "";
  char date[32] = "";    // raw +CMGR date field
  char text[1024] = "";  // UTF-8 decoded from UCS2 hex
  char storage[3] = "";  // selected read/delete store: ME or SM
  bool concatComplete = true;
  char concatReceived[8] = "";
  char concatTotal[8] = "";
};
// #endregion STRUCT_ModemSms

// #region CONST_sendSegmentLimits
// PURPOSE: Gates send routing at the carrier segment the peer actually
// bills: a text beyond one segment is silently truncated by text mode
// (measured on device), so it must go out as UCS2 concat PDU parts within
// the shared 335-unit cap.
constexpr size_t kGsmSingleSegmentUnits = 160;
constexpr size_t kUcs2TextSegmentUnits = 70;
constexpr size_t kUcs2PduPartUnits = 67;  // 140 TP-UD octets: 6 UDH + 134 data
// Bounds only volatile inbound reassembly RAM.
constexpr size_t kMaxSmsMultipartParts = 5;
// Accommodates the 335-unit form cap when 67-unit boundaries must move back
// around surrogate pairs; does not change the inbound cache bound.
constexpr size_t kMaxSmsSendMultipartParts = 6;
// #endregion CONST_sendSegmentLimits

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
// PURPOSE: Sequences the AT dialog (init, pollStatus, and the SMS ops)
// and exposes failedStage/lastReply for observable Serial contracts.
class ModemClient {
 public:
  ModemClient(ModemChannel& channel, char* scratch, size_t scratchSize);

  // Bring-up: AT → ATE0 → ATV1 → AT+CMEE=2 → wait SMS DONE (bounded).
  ModemResult init();

  // Polls all status commands into out; present=false when AT not seen.
  ModemResult pollStatus(ModemStatus& out);

  // SMS lifecycle: bounded inbox scan, full-body read, delete, mixed send.
  ModemResult findOldestUnread(ModemSms& out, bool& found);
  // Lists incoming records after draining CMGL to terminal OK; does not
  // issue CMGR, so callers can skip concat parts already held in RAM.
  ModemResult findUnreadCandidates(struct ModemInboxCandidate* out, size_t capacity, size_t& count);
  ModemResult readSms(const char* id, ModemSms& out);
  // Temporarily enters PDU mode to inspect the SMS-DELIVER UDH, then
  // restores text mode on every outcome. The body remains text-mode CMGR.
  ModemResult probeConcat(const char* id, struct ModemConcatInfo& out);
  ModemResult deleteSms(const char* id);
  ModemResult sendSms(const char* number, const char* textUtf8);
  // Keeps incoming SMS landing in ME while the inbox scan may work on the
  // small SIM store: selects only the read/delete storage (mem1) for
  // CMGL/CMGR/CMGD ("ME" or "SM"); write and new-message storages
  // (mem2/mem3) always stay "ME".
  ModemResult selectReadStorage(const char* mem);

  const char* failedStage() const { return failedStage_; }
  const char* lastReply() const { return scratch_ ? scratch_ : ""; }

 private:
  ModemResult sendCommand(const char* cmd, unsigned long timeoutMs);
  ModemResult readResponse();
  // Shared CMGS tail (">" prompt, payload + Ctrl-Z, +CMGS/OK) reused by
  // text mode and every PDU part.
  ModemResult submitData(const char* payload);
  // Sends one text beyond a single segment as UCS2 concat PDU parts.
  ModemResult sendMultipartUcs2(const char* number, const char* textUtf8, size_t units);
  void fail(const char* stage);

  ModemChannel& channel_;
  char* scratch_;
  size_t scratchSize_;
  const char* failedStage_ = "";
  size_t replyLen_ = 0;
  uint8_t pduRef_ = 0;  // concat reference, increments per multipart send
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
struct ModemInboxCandidate {
  char id[16] = "";
};
// #region STRUCT_ModemConcatInfo
// PURPOSE: Carries concat metadata from the PDU probe to the bounded cache,
// so 8-bit and 16-bit UDH references remain distinct identities.
struct ModemConcatInfo {
  bool present = false;
  uint16_t ref = 0;
  uint8_t total = 1;
  uint8_t seq = 1;
  bool refIs16Bit = false;
};
// #endregion STRUCT_ModemConcatInfo
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
// True only when every byte sits in the ASCII subset whose codes equal
// GSM 03.38; gates the raw send path (everything else goes UCS2).
bool isDirectGsmAsciiText(const char* text);
// Builds one UCS2 SMS-SUBMIT PDU (TP-UDHI) for one concat part; writes the
// hex form into out and the TPDU octet count (SCA field excluded — the
// value AT+CMGS expects in PDU mode) into pduOctetsOut. Returns false on
// invalid input or when out is too small.
bool buildUcs2SubmitPdu(const char* number, const char* partUcs2Hex, uint8_t ref, uint8_t total,
                        uint8_t seq, char* out, size_t outSize, size_t& pduOctetsOut);
// Parses exactly one UDH encoded as hex, walking bounded IEI/IEDL/data
// entries. Returns true only for one valid concat IE; out retains full
// reference width and total/sequence metadata.
bool parsePduConcat(const char* udhHex, ModemConcatInfo& out);
// Walks a complete SMS-DELIVER PDU to its TP-UDH and extracts only a real
// concatenation IE. Returns false for malformed PDU; a valid single-part PDU
// returns true with out.present=false.
bool extractConcatFromDeliverPdu(const char* pduHex, ModemConcatInfo& out);
#endif  // MODEM_MODEM_CLIENT_H
