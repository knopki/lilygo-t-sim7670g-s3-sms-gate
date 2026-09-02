// #region MODULE_CONTRACT
// PURPOSE: Keeps SIM7670G SMS dialogs testable and bounded off hardware.
// SCOPE:
// - AT status polling, text/PDU SMS read/send/delete, concat metadata,
// - pure response parsers.
// - NOT: transport ownership, SMTP, HTTP, or NVS.
// INVARIANTS:
// - Channel is idle on return;
// - deletes follow SMTP acceptance;
// - read storage may switch but receive storage stays ME;
// - text mode is restored.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_CLIENT_H
#define MODEM_MODEM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

// #region ENUM_ModemResult
// PURPOSE: Keeps modem failures classifiable without exposing raw replies.
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
// PURPOSE: Gives HTTP and poll tasks one safe, consistent modem snapshot.
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

// #region FUNC_isModemNetworkRegistered
// PURPOSE: Shares the home-or-roaming registration gate required before
// trusting network-provided modem data such as NITZ.
bool isModemNetworkRegistered(const ModemStatus& status);
// #endregion FUNC_isModemNetworkRegistered

// #region STRUCT_ModemSms
// PURPOSE: Keeps modem SMS compatible with the shared forwarding pipeline.
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
// PURPOSE: Prevents send paths from silently truncating long SMS.
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
// PURPOSE: Keeps AT protocol tests independent from Serial1 hardware.
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
// PURPOSE: Keeps modem dialogs bounded, observable, and hardware-independent.
class ModemClient {
 public:
  // #region METHOD_ModemClient_ModemClient
  // PURPOSE: Gives each dialog an isolated channel and bounded workspace.
  ModemClient(ModemChannel& channel, char* scratch, size_t scratchSize);
  // #endregion METHOD_ModemClient_ModemClient

  // #region METHOD_ModemClient_init
  // PURPOSE: Establishes the modem state required for reliable SMS work.
  ModemResult init();
  // #endregion METHOD_ModemClient_init

  // #region METHOD_ModemClient_pollStatus
  // PURPOSE: Supplies status consumers without sharing live AT I/O.
  ModemResult pollStatus(ModemStatus& out);
  // #endregion METHOD_ModemClient_pollStatus

  // #region METHOD_ModemClient_findOldestUnread
  // PURPOSE: Lets forwarding inspect the oldest message before deletion.
  ModemResult findOldestUnread(ModemSms& out, bool& found);
  // #endregion METHOD_ModemClient_findOldestUnread

  // Lists incoming records after draining CMGL to terminal OK; does not
  // issue CMGR, so callers can skip concat parts already held in RAM.
  // #region METHOD_ModemClient_findUnreadCandidates
  // PURPOSE: Lets concat assembly avoid rereading cached message parts.
  ModemResult findUnreadCandidates(struct ModemInboxCandidate* out, size_t capacity, size_t& count);
  // #endregion METHOD_ModemClient_findUnreadCandidates

  // #region METHOD_ModemClient_readSms
  // PURPOSE: Supplies forwarding with one complete message snapshot.
  ModemResult readSms(const char* id, ModemSms& out);
  // #endregion METHOD_ModemClient_readSms
  // Temporarily enters PDU mode to inspect the SMS-DELIVER UDH, then
  // restores text mode on every outcome. The body remains text-mode CMGR.
  // #region METHOD_ModemClient_probeConcat
  // PURPOSE: Enables multipart assembly without leaving the modem in PDU mode.
  ModemResult probeConcat(const char* id, struct ModemConcatInfo& out);
  // #endregion METHOD_ModemClient_probeConcat

  // #region METHOD_ModemClient_deleteSms
  // PURPOSE: Prevents loss by deleting only an accepted message.
  ModemResult deleteSms(const char* id);
  // #endregion METHOD_ModemClient_deleteSms

  // #region METHOD_ModemClient_sendSms
  // PURPOSE: Sends text without carrier-side truncation or encoding loss.
  ModemResult sendSms(const char* number, const char* textUtf8);
  // #endregion METHOD_ModemClient_sendSms
  // Keeps incoming SMS landing in ME while the inbox scan may work on the
  // small SIM store: selects only the read/delete storage (mem1) for
  // CMGL/CMGR/CMGD ("ME" or "SM"); write and new-message storages
  // (mem2/mem3) always stay "ME".
  // #region METHOD_ModemClient_selectReadStorage
  // PURPOSE: Supports store fallback without redirecting incoming messages.
  ModemResult selectReadStorage(const char* mem);
  // #endregion METHOD_ModemClient_selectReadStorage

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
  // Applied to every line of the reply started by the latest command.
  unsigned long responseTimeoutMs_ = 1000;
  uint8_t pduRef_ = 0;  // concat reference, increments per multipart send
};
// #endregion CLASS_ModemClient

// Pure parsers exposed for host tests (like zte_client helpers).
// #region FUNC_parseCpinLine
// PURPOSE: Exposes SIM readiness without leaking raw modem replies.
bool parseCpinLine(const char* line, char* out, size_t outSize);
// #endregion FUNC_parseCpinLine

// #region FUNC_parseCsqLine
// PURPOSE: Normalizes signal readings for status and retry decisions.
bool parseCsqLine(const char* line, int& rssi, int& ber);
// #endregion FUNC_parseCsqLine

// #region FUNC_parseCesqLine
// PURPOSE: Normalizes LTE signal readings while preserving unknown sentinels.
bool parseCesqLine(const char* line, int& rsrpDbm, int& rsrqDb);
// #endregion FUNC_parseCesqLine

// #region FUNC_parseCregLine
// PURPOSE: Exposes registration state in a stable numeric form.
bool parseCregLine(const char* line, int& stat);
// #endregion FUNC_parseCregLine

// #region FUNC_parseCopsLine
// PURPOSE: Extracts operator identity without retaining modem buffers.
bool parseCopsLine(const char* line, char* op, size_t opSize, int& act);
// #endregion FUNC_parseCopsLine

// #region FUNC_parseCpmsLine
// PURPOSE: Exposes storage occupancy for safe inbox polling.
bool parseCpmsLine(const char* line, uint16_t& used, uint16_t& total);
// #endregion FUNC_parseCpmsLine

// #region FUNC_parseCclkLine
// PURPOSE: Preserves the modem clock text for status and time sync.
bool parseCclkLine(const char* line, char* out, size_t outSize);
// #endregion FUNC_parseCclkLine

// #region FUNC_cclkToEpochMs
// PURPOSE: Converts modem clock text into UTC for time-source arbitration.
bool cclkToEpochMs(const char* cclk, int64_t& epochMsOut);
// #endregion FUNC_cclkToEpochMs

// #region FUNC_parseImeiLine
// PURPOSE: Extracts a bounded device identity for operator diagnostics.
bool parseImeiLine(const char* line, char* out, size_t outSize);
// #endregion FUNC_parseImeiLine

// #region FUNC_parseFwLine
// PURPOSE: Extracts bounded firmware identity for operator diagnostics.
bool parseFwLine(const char* line, char* out, size_t outSize);
// #endregion FUNC_parseFwLine
// #region STRUCT_ModemCmglInfo
// PURPOSE: Carries parsed CMGL metadata without retaining modem buffers.
struct ModemCmglInfo {
  uint16_t idx = 0;
  char stat[16] = "";
  char oa[64] = "";  // decoded UTF-8 (from UCS2 hex when needed)
  char scts[32] = "";
  bool hasTail = false;
  int tooa = -1;
  int msgLen = -1;
};
// #endregion STRUCT_ModemCmglInfo

// #region STRUCT_ModemInboxCandidate
// PURPOSE: Carries one bounded inbox ID into concat discovery.
struct ModemInboxCandidate {
  char id[16] = "";
};
// #endregion STRUCT_ModemInboxCandidate
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

// #region STRUCT_ModemCmgrInfo
// PURPOSE: Carries parsed CMGR metadata for SMS reconstruction.
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
// #endregion STRUCT_ModemCmgrInfo

// #region FUNC_parseCmglHeader
// PURPOSE: Preserves inbox metadata needed before a body read.
bool parseCmglHeader(const char* line, ModemCmglInfo& out);
// #endregion FUNC_parseCmglHeader

// #region FUNC_parseCmgrHeader
// PURPOSE: Preserves body metadata needed for SMS reconstruction.
bool parseCmgrHeader(const char* line, ModemCmgrInfo& out);
// #endregion FUNC_parseCmgrHeader

// #region FUNC_parseCmglEntry
// PURPOSE: Keeps CMGL-derived SMS metadata bounded before a full body read.
bool parseCmglEntry(const char* headerLine, const char* bodyLine, ModemSms& out);
// #endregion FUNC_parseCmglEntry

// #region FUNC_parseCmgrEntry
// PURPOSE: Preserves the complete body for forwarding without trusting truncated listing text.
bool parseCmgrEntry(const char* headerLine, const char* bodyLine, ModemSms& out);
// #endregion FUNC_parseCmgrEntry

// #region FUNC_decodeModemText
// PURPOSE: Converts modem text into the shared UTF-8 SMS representation.
bool decodeModemText(const char* encoded, char* out, size_t outSize);
// #endregion FUNC_decodeModemText

// #region FUNC_isDirectGsmAsciiText
// PURPOSE: Selects the lossless single-segment GSM send path.
bool isDirectGsmAsciiText(const char* text);
// #endregion FUNC_isDirectGsmAsciiText

// #region FUNC_buildUcs2SubmitPdu
// PURPOSE: Keeps long SMS submission bounded and reassemblable.
bool buildUcs2SubmitPdu(const char* number, const char* partUcs2Hex, uint8_t ref, uint8_t total,
                        uint8_t seq, char* out, size_t outSize, size_t& pduOctetsOut);
// #endregion FUNC_buildUcs2SubmitPdu

// #region FUNC_parsePduConcat
// PURPOSE: Extracts concat metadata while preserving reference width.
bool parsePduConcat(const char* udhHex, ModemConcatInfo& out);
// #endregion FUNC_parsePduConcat

// #region FUNC_extractConcatFromDeliverPdu
// PURPOSE: Finds concat metadata in a complete SMS-DELIVER PDU.
bool extractConcatFromDeliverPdu(const char* pduHex, ModemConcatInfo& out);
// #endregion FUNC_extractConcatFromDeliverPdu
#endif  // MODEM_MODEM_CLIENT_H
