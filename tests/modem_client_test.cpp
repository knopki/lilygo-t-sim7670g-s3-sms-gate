// #region MODULE_CONTRACT
// PURPOSE: Keeps SIM7670G AT-dialog regressions reproducible without hardware.
// SCOPE:
// - Covers parsing, polling, storage selection, and exact SMS payloads;
// - NOT: UART timing or carrier delivery.
// INVARIANTS: Fake violations are reported to assertions, never hidden.
// #endregion MODULE_CONTRACT

#include "../sms_gate/codec.h"
#include "../sms_gate/modem/concat_cache.h"
#include "../sms_gate/modem/modem_client.h"
#include "../sms_gate/modem/modem_record.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// #region CLASS_FakeModemChannel
// PURPOSE: Makes command order, reply draining, and CMGS payloads observable
// without hiding fake violations.
class FakeModemChannel : public ModemChannel {
 public:
  struct Script {
    std::string expectCmd;
    std::vector<std::string> replyLines;
  };

  void addScript(const std::string& cmd, std::vector<std::string> lines) {
    scripts_.push_back({cmd, std::move(lines)});
  }

  bool write(const char* data, size_t len) override {
    rawWrites_.emplace_back(data, len);
    if (dataPhase_) {
      payload_.append(data, len);
      if (len > 0 && data[len - 1] == '\x1A') dataPhase_ = false;  // Ctrl-Z ends the data phase
      return true;
    }
    std::string cmd(data, len);
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();
    if (pendingReply()) {
      violate("command before reply drained: " + cmd +
              " (pending reply of: " + scripts_[scriptIdx_].expectCmd + ")");
      return true;
    }
    if (scriptIdx_ >= scripts_.size()) {
      violate("unexpected command, no script left: " + cmd);
      return true;
    }
    if (scripts_[scriptIdx_].expectCmd != cmd) {
      violate("command mismatch: " + cmd + " (expected: " + scripts_[scriptIdx_].expectCmd + ")");
      return true;
    }
    matched_.push_back(cmd);
    pending_ = true;
    lineIdx_ = 0;
    return true;
  }

  int readLine(char* buffer, size_t size, unsigned long timeoutMs) override {
    readTimeouts_.push_back(timeoutMs);
    if (broken_ || !pending_ || scriptIdx_ >= scripts_.size()) return -1;
    const Script& sc = scripts_[scriptIdx_];
    if (lineIdx_ >= sc.replyLines.size()) {
      // Scripted reply exhausted without a terminal line: modem silence.
      finishScript();
      return -1;
    }
    const std::string& line = sc.replyLines[lineIdx_];
    if (line.size() + 1 > size) return -1;
    ++lineIdx_;
    if (lineIdx_ >= sc.replyLines.size()) finishScript();
    memcpy(buffer, line.c_str(), line.size() + 1);
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
      trimmed.pop_back();
    if (trimmed == ">") dataPhase_ = true;  // CMGS prompt: next writes are payload
    return static_cast<int>(line.size());
  }

  void purge() override {
    // Keeps pending scripted lines on purpose: losing an undrained reply is
    // exactly what the "command before reply drained" violation must catch.
  }

  bool broken() const { return broken_; }
  const std::vector<std::string>& violations() const { return violations_; }
  const std::vector<std::string>& matchedCommands() const { return matched_; }
  const std::vector<std::string>& rawWrites() const { return rawWrites_; }
  const std::vector<unsigned long>& readTimeouts() const { return readTimeouts_; }
  const std::string& dataPayload() const { return payload_; }

 private:
  bool pendingReply() const {
    return pending_ && scriptIdx_ < scripts_.size() &&
           lineIdx_ < scripts_[scriptIdx_].replyLines.size();
  }
  void finishScript() {
    pending_ = false;
    ++scriptIdx_;
    lineIdx_ = 0;
  }
  void violate(const std::string& what) {
    violations_.push_back(what);
    broken_ = true;
  }

  std::vector<Script> scripts_;
  size_t scriptIdx_ = 0;
  size_t lineIdx_ = 0;
  bool pending_ = false;
  bool dataPhase_ = false;
  bool broken_ = false;
  std::vector<std::string> matched_;
  std::vector<std::string> rawWrites_;
  std::vector<unsigned long> readTimeouts_;
  std::vector<std::string> violations_;
  std::string payload_;
};
// #endregion CLASS_FakeModemChannel

// #region FUNC_expectNoViolations
// PURPOSE: Reports fake protocol violations before failing the owning test.
static void expectNoViolations(const FakeModemChannel& ch, const char* context) {
  if (ch.broken()) {
    for (const std::string& v : ch.violations())
      fprintf(stderr, "fake violation [%s]: %s\n", context, v.c_str());
  }
  assert(!ch.broken());
}
// #endregion FUNC_expectNoViolations

// #region FUNC_fakeSentCommand
// PURPOSE: Checks whether a scripted command was actually matched.
static bool fakeSentCommand(const FakeModemChannel& ch, const std::string& cmd) {
  for (const std::string& c : ch.matchedCommands())
    if (c == cmd) return true;
  return false;
}
// #endregion FUNC_fakeSentCommand

// #region FUNC_fakePayloadPart
// PURPOSE: Keeps multipart assertions tied to exact captured CMGS parts.
static std::string fakePayloadPart(const FakeModemChannel& ch, size_t idx) {
  std::vector<std::string> parts;
  std::string cur;
  for (const char c : ch.dataPayload()) {
    cur += c;
    if (c == '\x1A') {
      parts.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return idx < parts.size() ? parts[idx] : std::string();
}
// #endregion FUNC_fakePayloadPart

// #region FUNC_testParseCpin
// PURPOSE: Keeps CPIN status parsing aligned with modem reply shapes.
void testParseCpin() {
  char out[32];
  assert(parseCpinLine("+CPIN: READY", out, sizeof(out)) && strcmp(out, "READY") == 0);
  assert(parseCpinLine("+CPIN: NOT INSERTED", out, sizeof(out)) &&
         strcmp(out, "NOT INSERTED") == 0);
  assert(parseCpinLine("+CPIN: SIM PIN", out, sizeof(out)) && strcmp(out, "SIM PIN") == 0);
  assert(!parseCpinLine("OK", out, sizeof(out)));
}
// #endregion FUNC_testParseCpin

// #region FUNC_testParseCsq
// PURPOSE: Keeps RSSI and BER parsing aligned with modem reply shapes.
void testParseCsq() {
  int rssi, ber;
  assert(parseCsqLine("+CSQ: 22,0", rssi, ber) && rssi == 22 && ber == 0);
  assert(parseCsqLine("+CSQ: 99,99", rssi, ber) && rssi == 99);
  assert(!parseCsqLine("+CSQ: ", rssi, ber));
}
// #endregion FUNC_testParseCsq

// #region FUNC_testParseCesq
// PURPOSE: Preserves LTE signal conversion and unknown-value handling.
void testParseCesq() {
  int rsrp, rsrq;
  assert(parseCesqLine("+CESQ: 99,99,255,255,30,60", rsrp, rsrq));
  assert(rsrp == -80);  // -140+60
  assert(parseCesqLine("+CESQ: 99,99,255,255,255,255", rsrp, rsrq) && rsrp == 0);
}
// #endregion FUNC_testParseCesq

// #region FUNC_testParseCreg
// PURPOSE: Preserves registration-state extraction across CEREG and CREG.
void testParseCreg() {
  int stat;
  assert(parseCregLine("+CEREG: 0,1", stat) && stat == 1);
  assert(parseCregLine("+CREG: 0,5", stat) && stat == 5);
  assert(parseCregLine("+CEREG: 0,6", stat) && stat == 6);
}
// #endregion FUNC_testParseCreg

// #region FUNC_testNetworkRegistrationGate
// PURPOSE: Keeps NITZ blocked until either circuit or packet registration is home or roaming.
void testNetworkRegistrationGate() {
  ModemStatus status{};
  assert(!isModemNetworkRegistered(status));
  status.ceregStat = 1;
  assert(isModemNetworkRegistered(status));
  status.ceregStat = 0;
  status.cregStat = 5;
  assert(isModemNetworkRegistered(status));
  status.cregStat = 2;
  assert(!isModemNetworkRegistered(status));
}
// #endregion FUNC_testNetworkRegistrationGate

// #region FUNC_testParseCops
// PURPOSE: Preserves operator and access-technology extraction.
void testParseCops() {
  char op[32];
  int act;
  assert(parseCopsLine("+COPS: 0,2,\"25020\",7", op, sizeof(op), act) && strcmp(op, "25020") == 0 &&
         act == 7);
  assert(parseCopsLine("+COPS: 0", op, sizeof(op), act));
}
// #endregion FUNC_testParseCops

// #region FUNC_testParseCpms
// PURPOSE: Preserves SMS storage counters needed by polling.
void testParseCpms() {
  uint16_t used, total;
  assert(parseCpmsLine("+CPMS: \"ME\",3,100,\"ME\",3,100,\"ME\",3,100", used, total) && used == 3 &&
         total == 100);
  assert(parseCpmsLine("+CPMS: \"SM\",0,30,\"SM\",0,30,\"SM\",0,30", used, total) && total == 30);
}
// #endregion FUNC_testParseCpms

// #region FUNC_testParseCclk
// PURPOSE: Preserves modem clock extraction for time synchronization.
void testParseCclk() {
  char out[32];
  assert(parseCclkLine("+CCLK: \"25/08/25,12:34:56+12\"", out, sizeof(out)) &&
         strcmp(out, "25/08/25,12:34:56+12") == 0);
}
// #endregion FUNC_testParseCclk

// #region FUNC_testParseImei
// PURPOSE: Rejects malformed identity replies before they reach status output.
void testParseImei() {
  char out[24];
  assert(parseImeiLine("864567789012345", out, sizeof(out)) && strcmp(out, "864567789012345") == 0);
  assert(parseImeiLine("+CGSN: 864567789012345", out, sizeof(out)) &&
         strcmp(out, "864567789012345") == 0);
  assert(!parseImeiLine("ERROR", out, sizeof(out)));
  assert(!parseImeiLine("123", out, sizeof(out)));
}
// #endregion FUNC_testParseImei

// #region FUNC_testParseFw
// PURPOSE: Preserves firmware-version extraction used for diagnostics.
void testParseFw() {
  char out[48];
  assert(parseFwLine("2374B03SIM767XM5A_M", out, sizeof(out)) &&
         strcmp(out, "2374B03SIM767XM5A_M") == 0);
  assert(parseFwLine("+CGMR: 2374B03SIM767XM5A_M", out, sizeof(out)) &&
         strcmp(out, "2374B03SIM767XM5A_M") == 0);
}
// #endregion FUNC_testParseFw

// #region FUNC_makeModemSourceRecord
// PURPOSE: Provides one valid record baseline so mutations isolate validation rules.
ModemSourceRecord makeModemRecord() {
  ModemSourceRecord record{};
  record.magic = kModemSourceMagic;
  record.version = kModemSourceVersion;
  record.moduleEnabled = 1;
  record.pollEnabled = 1;
  record.pollIntervalSec = kDefaultModemPollSec;
  strcpy(record.label, "+79990000001");
  record.nitzTimeSyncEnabled = 1;
  record.smsPollEnabled = 1;
  record.checksum = calculateModemSourceChecksum(record);
  return record;
}
// #endregion FUNC_makeModemSourceRecord

// #region FUNC_testModemRecordValidation
// PURPOSE: Ensures invalid modem-source records never reach polling.
void testModemRecordValidation() {
  ModemSourceRecord record = makeModemRecord();
  assert(isModemSourceRecordValid(record));

  // Empty label is valid.
  record = makeModemRecord();
  record.label[0] = '\0';
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));

  // Max-length printable label (31 chars) is valid.
  record = makeModemRecord();
  memset(record.label, 'A', kMaxModemLabelLength);
  record.label[kMaxModemLabelLength] = '\0';
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));

  // Non-printable label rejected.
  record = makeModemRecord();
  strcpy(record.label, "bad\x01label");
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  // Unterminated label (no NUL within 31) rejected.
  record = makeModemRecord();
  memset(record.label, 'B', sizeof(record.label));
  // No NUL
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.checksum ^= 1;
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.version = static_cast<uint16_t>(kModemSourceVersion + 1);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.magic = 0x11111111;
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.moduleEnabled = 7;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.pollEnabled = 7;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.smsPollEnabled = 7;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.moduleEnabled = 0;
  record.pollEnabled = 0;
  record.smsPollEnabled = 0;
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));

  // Interval boundaries
  record = makeModemRecord();
  record.pollIntervalSec = kMinModemPollSec;
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));

  record.pollIntervalSec = kMaxModemPollSec;
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));

  record.pollIntervalSec = kMinModemPollSec - 1;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.pollIntervalSec = kMaxModemPollSec + 1;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.pollIntervalSec = 0;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  assert(isValidModemPollInterval(kMinModemPollSec));
  assert(isValidModemPollInterval(kDefaultModemPollSec));
  assert(isValidModemPollInterval(kMaxModemPollSec));
  assert(!isValidModemPollInterval(kMinModemPollSec - 1));
  assert(!isValidModemPollInterval(kMaxModemPollSec + 1));
  assert(!isValidModemPollInterval(0));

  record = makeModemRecord();
  record.nitzTimeSyncEnabled = 0;
  record.checksum = calculateModemSourceChecksum(record);
  assert(isModemSourceRecordValid(record));
  record.nitzTimeSyncEnabled = 2;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  // V2 migration sample
  ModemSourceRecordV2 v2{};
  v2.magic = kModemSourceMagic;
  v2.version = 2;
  v2.enabled = 1;
  v2.pollIntervalSec = kDefaultModemPollSec;
  strcpy(v2.label, "+79990000001");
  v2.nitzTimeSyncEnabled = 1;
  v2.checksum = calculateModemSourceV2Checksum(v2);
  assert(isModemSourceRecordV2Valid(v2));
  v2.label[0] = 'X';
  assert(v2.checksum != calculateModemSourceV2Checksum(v2));

  puts("testModemRecordValidation ok");
}
// #endregion FUNC_testModemRecordValidation

// #region FUNC_testPollStatus
// PURPOSE: Pins the status command order and storage-selection contract.
void testPollStatus() {
  FakeModemChannel ch;
  char scratch[256];
  // Script in order pollStatus expects
  ch.addScript("AT", {"AT", "OK"});
  ch.addScript("AT+CPIN?", {"+CPIN: READY", "OK"});
  ch.addScript("AT+CSQ", {"+CSQ: 22,0", "OK"});
  ch.addScript("AT+CESQ", {"+CESQ: 99,99,255,255,30,60", "OK"});
  ch.addScript("AT+CEREG?", {"+CEREG: 0,1", "OK"});
  ch.addScript("AT+CREG?", {"+CREG: 0,1", "OK"});
  ch.addScript("AT+CGATT?", {"+CGATT: 1", "OK"});
  ch.addScript("AT+COPS?", {"+COPS: 0,2,\"25020\",7", "OK"});
  ch.addScript("AT+CPMS?", {"+CPMS: \"ME\",3,100,\"ME\",3,100,\"ME\",3,100", "OK"});
  // Reading SM counters must switch only mem1; incoming storage stays ME.
  ch.addScript("AT+CPMS=\"SM\",\"ME\",\"ME\"",
               {"+CPMS: \"SM\",1,30,\"ME\",3,100,\"ME\",3,100", "OK"});
  ch.addScript("AT+CPMS?", {"+CPMS: \"SM\",1,30,\"ME\",3,100,\"ME\",3,100", "OK"});
  ch.addScript("AT+CPMS=\"ME\",\"ME\",\"ME\"", {"OK"});
  ch.addScript("AT+CCLK?", {"+CCLK: \"25/08/25,12:34:56+12\"", "OK"});
  ch.addScript("AT+CGSN", {"864567789012345", "OK"});
  ch.addScript("AT+CGMR", {"+CGMR: 2374B03SIM767XM5A_M", "OK"});

  ModemClient client(ch, scratch, sizeof(scratch));
  ModemStatus st;
  ModemResult r = client.pollStatus(st);
  expectNoViolations(ch, "poll_status");
  assert(r == ModemResult::kSuccess);
  assert(st.present);
  assert(strcmp(st.cpin, "READY") == 0);
  assert(st.csqRssi == 22);
  assert(st.ceregStat == 1);
  assert(st.cgatt);
  assert(strcmp(st.copsOp, "25020") == 0);
  assert(st.smsUsedMe == 3 && st.smsTotalMe == 100);
  assert(st.smsUsedSm == 1 && st.smsTotalSm == 30);
  assert(strcmp(st.imei, "864567789012345") == 0);
  assert(strcmp(st.fw, "2374B03SIM767XM5A_M") == 0);
}
// #endregion FUNC_testPollStatus

// #region FUNC_testCodecUcs2
// PURPOSE: Keeps UCS-2 validation and UTF-8 decoding interoperable.
void testCodecUcs2() {
  assert(codec::isUcs2HexView("00480065006C006C006F", 20));
  assert(!codec::isUcs2HexView("004G", 4));
  assert(!codec::isUcs2HexView("004", 3));
  assert(codec::isUcs2HexView("", 0));
  char out[32] = "";
  codec::decodeUcs2HexView("00480065006C006C006F", 20, out, sizeof(out));
  assert(strcmp(out, "Hello") == 0);
  codec::decodeUcs2HexView("041F04400438043204350442", 24, out, sizeof(out));
  // "Привет" in UTF-8
  assert(strcmp(out, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == 0);
  // Surrogate pair D83D DE00 -> U+1F600
  codec::decodeUcs2HexView("D83DDE00", 8, out, sizeof(out));
  assert(strcmp(out, "\xF0\x9F\x98\x80") == 0);
  // Plain GSM stays not hex
  assert(!codec::isUcs2HexView("Hello", 5));
  puts("testCodecUcs2 ok");
}
// #endregion FUNC_testCodecUcs2

// #region FUNC_testParseCmglHeader
// PURPOSE: Preserves CMGL header fields across text and UCS-2 forms.
void testParseCmglHeader() {
  ModemCmglInfo info{};
  assert(parseCmglHeader(
      "+CMGL: 1,\"REC "
      "UNREAD\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/25,20:29:40+12\"",
      info));
  assert(info.idx == 1);
  assert(strcmp(info.stat, "REC UNREAD") == 0);
  assert(strcmp(info.oa, "+79685557161") == 0);
  assert(strcmp(info.scts, "26/08/25,20:29:40+12") == 0);
  assert(!info.hasTail);
  assert(
      parseCmglHeader("+CMGL: 1,\"REC "
                      "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
                      "25,20:29:40+12\",145,15",
                      info));
  assert(info.hasTail && info.tooa == 145 && info.msgLen == 15);
  // GSM plain OA
  assert(parseCmglHeader("+CMGL: 0,\"REC UNREAD\",\"+79685557161\",\"\",\"26/08/25,20:31:14+12\"",
                         info));
  assert(strcmp(info.oa, "+79685557161") == 0);
  assert(!info.hasTail);
  // Malformed
  assert(!parseCmglHeader("OK", info));
  assert(!parseCmglHeader("+CMGL: bad", info));
  puts("testParseCmglHeader ok");
}
// #endregion FUNC_testParseCmglHeader

// #region FUNC_testParseCmgrHeader
// PURPOSE: Preserves CMGR header fields and optional PDU metadata.
void testParseCmgrHeader() {
  ModemCmgrInfo info{};
  assert(parseCmgrHeader(
      "+CMGR: \"REC "
      "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/25,16:59:23+12\"",
      info));
  assert(strcmp(info.stat, "REC READ") == 0);
  assert(strcmp(info.oa, "+79685557161") == 0);
  assert(!info.hasTail);
  assert(parseCmgrHeader(
      "+CMGR: \"REC "
      "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
      "25,16:59:23+12\",145,4,0,8,\"002B00370039003600390033003500300031003300350037\",145,5",
      info));
  assert(info.hasTail);
  assert(info.tooa == 145 && info.fo == 4 && info.pid == 0 && info.dcs == 8);
  assert(strcmp(info.sca, "+79693501357") == 0);
  assert(info.tosca == 145 && info.msgLen == 5);
  assert(!parseCmgrHeader("ERROR", info));
  puts("testParseCmgrHeader ok");
}
// #endregion FUNC_testParseCmgrHeader

// #region FUNC_testParseCmglEntry
// PURPOSE: Preserves CMGL body decoding and concatenation metadata.
void testParseCmglEntry() {
  ModemSms sms{};
  // Cyrillic "Привет тест 123" from research §2
  assert(parseCmglEntry(
      "+CMGL: 1,\"REC "
      "UNREAD\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/25,20:29:40+12\"",
      "041F04400438043204350442002004420435044104420020003100320033", sms));
  assert(strcmp(sms.id, "1") == 0);
  assert(strcmp(sms.number, "+79685557161") == 0);
  assert(strcmp(sms.date, "26/08/25,20:29:40+12") == 0);
  assert(strcmp(sms.text,
                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD1\x82\xD0\xB5\xD1\x81\xD1\x82 "
                "123") == 0);
  assert(sms.concatComplete);
  // GSM plain body
  assert(parseCmglEntry("+CMGL: 0,\"REC UNREAD\",\"+79685557161\",\"\",\"26/08/25,20:31:14+12\"",
                        "Hello test 123", sms));
  assert(strcmp(sms.text, "Hello test 123") == 0);
  assert(strcmp(sms.number, "+79685557161") == 0);
  // UCS2 "Hello" hex
  assert(parseCmglEntry(
      "+CMGL: 0,\"REC "
      "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/25,20:31:14+12\"",
      "00480065006C006C006F002000740065007300740020003100320033", sms));
  assert(strcmp(sms.text, "Hello test 123") == 0);
  puts("testParseCmglEntry ok");
}
// #endregion FUNC_testParseCmglEntry

// #region FUNC_testParseCmgrEntry
// PURPOSE: Preserves CMGR body decoding and concatenation metadata.
void testParseCmgrEntry() {
  ModemSms sms{};
  assert(parseCmgrEntry(
      "+CMGR: \"REC "
      "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
      "25,16:59:23+12\",145,4,0,8,\"002B00370039003600390033003500300031003300350037\",145,5",
      "0421043C0441043A0430", sms));
  assert(strcmp(sms.number, "+79685557161") == 0);
  assert(strcmp(sms.date, "26/08/25,16:59:23+12") == 0);
  // "!#CA0"? Actually 0421 043C 0441 043A 0430 -> decode
  assert(sms.text[0] != '\0');
  assert(sms.concatComplete);
  puts("testParseCmgrEntry ok");
}
// #endregion FUNC_testParseCmgrEntry

// #region FUNC_testParsePduConcat
// PURPOSE: Preserves 8-bit and 16-bit concatenation identity parsing.
void testParsePduConcat() {
  ModemConcatInfo concat{};
  assert(parsePduConcat("050003530401", concat));
  assert(concat.ref == 0x53 && !concat.refIs16Bit && concat.total == 4 && concat.seq == 1);
  assert(parsePduConcat("050003530403", concat) && concat.seq == 3);
  assert(parsePduConcat("06080412340201", concat));
  assert(concat.ref == 0x1234 && concat.refIs16Bit && concat.total == 2 && concat.seq == 1);
  assert(!parsePduConcat("00480065006C", concat));
  assert(!parsePduConcat("", concat));
  // Lower-case hex remains valid when it fills an exact UDH IE.
  assert(parsePduConcat("050003ab0201", concat));
  assert(concat.ref == 0xAB && !concat.refIs16Bit && concat.total == 2 && concat.seq == 1);
  puts("testParsePduConcat ok");
}
// #endregion FUNC_testParsePduConcat

// #region FUNC_testExtractConcatFromDeliverPdu
// PURPOSE: Keeps variable SMS-DELIVER offsets from misreading body data as concat metadata.
void testExtractConcatFromDeliverPdu() {
  ModemConcatInfo concat{};
  // SCA=0, SMS-DELIVER+UDHI, 11-digit OA, DCS=UCS2, 7-byte SCTS,
  // UDL=8, UDH 05 00 03 53 02 01, body U+0041.
  assert(extractConcatFromDeliverPdu("00440B912143658709F1000800000000000000080500035302010041",
                                     concat));
  assert(concat.present && concat.ref == 0x53 && concat.total == 2 && concat.seq == 1);
  // The OA length changes the UDH offset; eight digits are an exact byte boundary.
  assert(
      extractConcatFromDeliverPdu("0044089121436587000800000000000000080500035302020041", concat));
  assert(concat.present && concat.seq == 2);
  // A valid plain SMS-DELIVER has no UDH.
  assert(extractConcatFromDeliverPdu("00040B912143658709F1000800000000000000020041", concat));
  assert(!concat.present && concat.total == 1 && concat.seq == 1);
  // 050003 inside payload is body data, not a UDH.
  assert(
      extractConcatFromDeliverPdu("00040B912143658709F100080000000000000006050003530201", concat));
  assert(!concat.present);
  assert(!extractConcatFromDeliverPdu("00440B912143658709F100080000", concat));
  assert(!extractConcatFromDeliverPdu("00440B912143658709F10008000000000000000Z", concat));

  // A 16-bit concatenation IE retains both reference octets. A 16-bit
  // reference that shares the same high byte with another set must not merge.
  assert(extractConcatFromDeliverPdu("00440B912143658709F100080000000000000009060804123402010041",
                                     concat));
  assert(concat.present && concat.ref == 0x1234 && concat.refIs16Bit && concat.total == 2 &&
         concat.seq == 1);

  // A concat-looking sequence inside another valid UDH IE is not metadata.
  assert(extractConcatFromDeliverPdu(
      "00440B912143658709F10008000000000000000B0870060500035302010041", concat));
  assert(!concat.present);
  puts("testExtractConcatFromDeliverPdu ok");
}
// #endregion FUNC_testExtractConcatFromDeliverPdu

// #region FUNC_testProbeConcat
// PURPOSE: Ensures PDU probing restores text mode after success and errors.
void testProbeConcat() {
  char scratch[512];
  {
    FakeModemChannel ch;
    ch.addScript("AT+CMGF=0", {"OK"});
    ch.addScript("AT+CMGR=1", {"+CMGR: 0,,25",
                               "00440B912143658709F1000800000000000000080500035302010041", "OK"});
    ch.addScript("AT+CMGF=1", {"OK"});
    ModemClient client(ch, scratch, sizeof(scratch));
    ModemConcatInfo concat{};
    assert(client.probeConcat("1", concat) == ModemResult::kSuccess);
    expectNoViolations(ch, "probe_concat_success");
    assert(concat.present && concat.total == 2 && concat.seq == 1);
  }
  {
    FakeModemChannel ch;
    ch.addScript("AT+CMGF=0", {"OK"});
    ch.addScript("AT+CMGR=1", {"+CMS ERROR: 321"});
    ch.addScript("AT+CMGF=1", {"OK"});
    ModemClient client(ch, scratch, sizeof(scratch));
    ModemConcatInfo concat{};
    assert(client.probeConcat("1", concat) == ModemResult::kProtocolError);
    expectNoViolations(ch, "probe_concat_error_restore");
    assert(fakeSentCommand(ch, "AT+CMGF=1"));
  }
  puts("testProbeConcat ok");
}
// #endregion FUNC_testProbeConcat

// #region FUNC_testModemConcatCache
// PURPOSE: Keeps multipart reassembly, identity bounds, and retry state deterministic.
void testModemConcatCache() {
  auto sms = [](const char* id, const char* text) {
    ModemSms value{};
    snprintf(value.id, sizeof(value.id), "%s", id);
    snprintf(value.number, sizeof(value.number), "+70000000001");
    snprintf(value.text, sizeof(value.text), "%s", text);
    return value;
  };
  ModemConcatCache cache;
  ModemConcatInfo one{true, 0x53, 3, 1};
  ModemConcatInfo two{true, 0x53, 3, 2};
  ModemConcatInfo three{true, 0x53, 3, 3};
  assert(cache.store(sms("11", "first"), one));
  assert(cache.containsId("11", ""));
  size_t set = 0;
  assert(!cache.findComplete(set));
  cache.advanceCycle();
  assert(cache.store(sms("12", "second"), two));
  assert(cache.store(sms("13", "third"), three));
  assert(cache.findComplete(set) && cache.total(set) == 3);
  assert(strcmp(cache.part(set, 0)->text, "first") == 0);
  assert(strcmp(cache.part(set, 2)->id, "13") == 0);
  ModemSms joined;
  assert(cache.buildComplete(set, joined));
  assert(strcmp(joined.text, "firstsecondthird") == 0 && joined.concatComplete);
  assert(cache.markCompleteSmtpAccepted(set));
  assert(cache.markPartDeleted(set, 0));
  assert(cache.markPartDeleted(set, 1));
  assert(cache.markPartDeleted(set, 2));
  assert(cache.removable(set));
  cache.remove(set);
  assert(!cache.containsId("11", ""));

  // Two distinct sets fit; a third is retained in ME for a later cycle.
  ModemConcatInfo other{true, 0x54, 2, 1};
  ModemConcatInfo thirdSet{true, 0x55, 2, 1};
  assert(cache.store(sms("21", "a"), other));
  assert(cache.store(sms("22", "b"), thirdSet));
  assert(!cache.store(sms("23", "c"), ModemConcatInfo{true, 0x56, 2, 1}));
  assert(!cache.store(sms("24", "oversized"), ModemConcatInfo{true, 0x57, 6, 1}));

  ModemConcatCache timeout;
  assert(timeout.store(sms("31", "only"), ModemConcatInfo{true, 0x60, 2, 1}));
  for (int i = 0; i < 19; ++i) timeout.advanceCycle();
  assert(!timeout.findExpired(set));
  timeout.advanceCycle();
  assert(timeout.findExpired(set));
  assert(timeout.part(set, 0) != nullptr && timeout.part(set, 1) == nullptr);

  // Full 16-bit references with a shared high byte are separate identities.
  // Removing the completed 0x1234 set must retain the incomplete 0x12AB set.
  ModemConcatInfo first{};
  ModemConcatInfo second{};
  ModemConcatInfo firstLast{};
  assert(extractConcatFromDeliverPdu("00440B912143658709F100080000000000000009060804123402010041",
                                     first));
  assert(extractConcatFromDeliverPdu("00440B912143658709F10008000000000000000906080412AB02010042",
                                     second));
  assert(extractConcatFromDeliverPdu("00440B912143658709F100080000000000000009060804123402020043",
                                     firstLast));
  ModemConcatCache refs;
  assert(refs.store(sms("41", "first"), first));
  assert(refs.store(sms("51", "other"), second));
  assert(refs.store(sms("42", "last"), firstLast));
  assert(refs.findComplete(set));
  assert(refs.markCompleteSmtpAccepted(set));
  assert(refs.markPartDeleted(set, 0));
  assert(refs.markPartDeleted(set, 1));
  refs.remove(set);
  assert(refs.containsId("51", ""));

  // A 16-bit 0x0034 reference must not collide with its 8-bit 0x34 form.
  ModemConcatCache refWidth;
  assert(refWidth.store(sms("61", "eight"), ModemConcatInfo{true, 0x34, 2, 1, false}));
  assert(refWidth.store(sms("71", "sixteen"), ModemConcatInfo{true, 0x34, 2, 1, true}));
  assert(refWidth.containsId("61", "") && refWidth.containsId("71", ""));

  // SMTP acceptance is volatile but must suppress a repeat submission while
  // CMGD cleanup retries. Every present part remains accounted for until its
  // own deletion succeeds.
  ModemConcatCache cleanup;
  assert(cleanup.store(sms("81", "first"), ModemConcatInfo{true, 0x81, 2, 1}));
  assert(cleanup.store(sms("82", "second"), ModemConcatInfo{true, 0x81, 2, 2}));
  assert(cleanup.findComplete(set));
  assert(cleanup.completeReadyForSmtp(set));
  assert(cleanup.buildComplete(set, joined));
  assert(cleanup.markCompleteSmtpAccepted(set));
  assert(!cleanup.completeReadyForSmtp(set));
  assert(!cleanup.buildComplete(set, joined));
  assert(cleanup.partNeedsDelete(set, 0) && cleanup.partNeedsDelete(set, 1));
  assert(cleanup.markPartDeleted(set, 0));
  assert(!cleanup.partNeedsDelete(set, 0) && cleanup.partNeedsDelete(set, 1));
  assert(!cleanup.removable(set));
  assert(cleanup.markPartDeleted(set, 1));
  assert(cleanup.removable(set));
  cleanup.remove(set);
  assert(!cleanup.containsId("81", ""));

  // Expired incomplete parts carry their own SMTP/delete progress: a failed
  // CMGD can retry deletion without forwarding the fragment twice.
  ModemConcatCache expired;
  assert(expired.store(sms("91", "fragment"), ModemConcatInfo{true, 0x91, 2, 1}));
  for (int i = 0; i < 20; ++i) expired.advanceCycle();
  assert(expired.findExpired(set));
  assert(expired.partReadyForSmtp(set, 0));
  assert(expired.markPartSmtpAccepted(set, 0));
  assert(!expired.partReadyForSmtp(set, 0));
  assert(expired.partNeedsDelete(set, 0));
  assert(!expired.removable(set));
  assert(expired.markPartDeleted(set, 0));
  assert(expired.removable(set));
  puts("testModemConcatCache ok");
}
// #endregion FUNC_testModemConcatCache

// #region FUNC_testFindOldestUnreadUcs2
// PURPOSE: Verifies oldest unread selection and UCS-2 body decoding.
void testFindOldestUnreadUcs2() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGL=\"ALL\"",
               {"+CMGL: 1,\"REC "
                "UNREAD\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
                "25,20:29:40+12\"",
                "041F04400438043204350442002004420435044104420020003100320033", "OK"});
  ch.addScript(
      "AT+CMGR=1",
      {"+CMGR: \"REC "
       "UNREAD\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
       "25,20:29:40+12\",145,4,0,8,\"002B00370039003600390033003500300031003300350037\",145,15",
       "041F04400438043204350442002004420435044104420020003100320033", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  assert(strcmp(sms.id, "1") == 0);
  assert(strcmp(sms.number, "+79685557161") == 0);
  assert(strstr(sms.text, "\xD0\x9F") != nullptr);
  puts("testFindOldestUnreadUcs2 ok");
}
// #endregion FUNC_testFindOldestUnreadUcs2

// #region FUNC_testFindOldestUnreadGsm
// PURPOSE: Verifies oldest unread selection for plain GSM text.
void testFindOldestUnreadGsm() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGL=\"ALL\"",
               {"+CMGL: 0,\"REC UNREAD\",\"+79685557161\",\"\",\"26/08/25,20:31:14+12\"",
                "Hello test 123", "OK"});
  ch.addScript("AT+CMGR=0", {"+CMGR: \"REC UNREAD\",\"+79685557161\",\"\",\"26/08/25,20:31:14+12\"",
                             "Hello test 123", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  assert(strcmp(sms.text, "Hello test 123") == 0);
  puts("testFindOldestUnreadGsm ok");
}
// #endregion FUNC_testFindOldestUnreadGsm

// #region FUNC_testFindOldestUnreadOrdered
// PURPOSE: Ensures listing order does not replace chronological selection.
void testFindOldestUnreadOrdered() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript(
      "AT+CMGL=\"ALL\"",
      {"+CMGL: 5,\"REC UNREAD\",\"+70000000001\",\"\",\"26/08/25,20:31:58+12\",145,4", "0054",
       "+CMGL: 2,\"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:29:40+12\",145,4", "0041",
       "OK"});
  ch.addScript("AT+CMGR=2", {"+CMGR: \"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:29:40+12\"",
                             "0041", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  assert(strcmp(sms.id, "2") == 0);
  assert(strcmp(sms.text, "A") == 0);
  assert(sms.concatComplete);
  puts("testFindOldestUnreadOrdered ok");
}
// #endregion FUNC_testFindOldestUnreadOrdered

// #region FUNC_testFindOldestUnreadEmptyAndError
// PURPOSE: Distinguishes an empty inbox from a modem protocol failure.
void testFindOldestUnreadEmptyAndError() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGL=\"ALL\"", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = true;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && !found);
  // CMS ERROR path
  FakeModemChannel ch2;
  ch2.addScript("AT+CMGL=\"ALL\"", {"+CMS ERROR: 500"});
  ModemClient client2(ch2, scratch, sizeof(scratch));
  assert(client2.findOldestUnread(sms, found) == ModemResult::kProtocolError);
  assert(strcmp(client2.failedStage(), "cms_error") == 0);
  puts("testFindOldestUnreadEmptyAndError ok");
}
// #endregion FUNC_testFindOldestUnreadEmptyAndError

// #region FUNC_testFindOldestUnreadFullStorage
// PURPOSE: Ensures a full listing is drained before the selected SMS is read.
void testFindOldestUnreadFullStorage() {
  FakeModemChannel ch;
  char scratch[512];
  // 30 header/body pairs plus empty framing noise; the oldest (idx 10) is
  // emitted late so only a fully drained CMGL can pick it via CMGR.
  std::vector<std::string> cmgl;
  for (int i = 0; i < 30; ++i) {
    const int idx = (i < 10) ? 20 + i : (i < 20 ? 30 + (i - 10) : 10 + (i - 20));
    char header[128];
    snprintf(header, sizeof(header),
             "+CMGL: %d,\"REC UNREAD\",\"+70000000000\",\"\",\"26/08/25,20:%02d:00+12\"", idx, i);
    cmgl.push_back(header);
    cmgl.push_back("0041");
    if (i % 7 == 0) cmgl.emplace_back("");  // framing noise must not drain the read budget
  }
  cmgl.push_back("OK");
  ch.addScript("AT+CMGL=\"ALL\"", cmgl);
  ch.addScript(
      "AT+CMGR=10",
      {"+CMGR: \"REC UNREAD\",\"+70000000000\",\"\",\"26/08/25,20:00:00+12\"", "0041", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  const ModemResult r = client.findOldestUnread(sms, found);
  expectNoViolations(ch, "full_storage");
  assert(r == ModemResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "10") == 0);
  assert(strcmp(sms.text, "A") == 0);
  // CMGR is only allowed after the terminal OK of CMGL was read.
  assert(ch.matchedCommands().size() == 2);
  assert(fakeSentCommand(ch, "AT+CMGR=10"));
  puts("testFindOldestUnreadFullStorage ok");
}
// #endregion FUNC_testFindOldestUnreadFullStorage

// #region FUNC_testFindOldestUnreadMissingOk
// PURPOSE: Rejects incomplete listings so no unverified message is read.
void testFindOldestUnreadMissingOk() {
  FakeModemChannel ch;
  char scratch[512];
  // A reply without terminal OK must never count as a successful scan and
  // must never trigger CMGR.
  ch.addScript("AT+CMGL=\"ALL\"",
               {"+CMGL: 3,\"REC UNREAD\",\"+70000000003\",\"\",\"26/08/25,20:00:00+12\"", "0041",
                "+CMGL: 1,\"REC UNREAD\",\"+70000000001\",\"\",\"26/08/25,20:01:00+12\"", "0042"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = true;
  assert(client.findOldestUnread(sms, found) == ModemResult::kTimeout);
  assert(!found);
  assert(!fakeSentCommand(ch, "AT+CMGR=1") && !fakeSentCommand(ch, "AT+CMGR=3"));
  assert(!ch.broken());  // no follow-up command was issued at all
  puts("testFindOldestUnreadMissingOk ok");
}
// #endregion FUNC_testFindOldestUnreadMissingOk

// #region FUNC_testFindOldestUnreadRecRead
// PURPOSE: Re-delivers read messages left behind by a failed forward.
void testFindOldestUnreadRecRead() {
  FakeModemChannel ch;
  char scratch[512];
  // REC READ left over after an SMTP failure must be re-delivered.
  ch.addScript(
      "AT+CMGL=\"ALL\"",
      {"+CMGL: 4,\"REC READ\",\"+70000000004\",\"\",\"26/08/25,20:00:00+12\"", "0041",
       "+CMGL: 2,\"REC READ\",\"+70000000002\",\"\",\"26/08/25,20:01:00+12\"", "0042", "OK"});
  ch.addScript("AT+CMGR=2", {"+CMGR: \"REC READ\",\"+70000000002\",\"\",\"26/08/25,20:01:00+12\"",
                             "0042", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  expectNoViolations(ch, "rec_read");
  assert(strcmp(sms.id, "2") == 0);
  puts("testFindOldestUnreadRecRead ok");
}
// #endregion FUNC_testFindOldestUnreadRecRead

// #region FUNC_testFindOldestUnreadIgnoresOutgoing
// PURPOSE: Ensures outgoing storage records never enter the receive queue.
void testFindOldestUnreadIgnoresOutgoing() {
  FakeModemChannel ch;
  char scratch[512];
  // Outgoing STO SENT/UNSENT records are ignored; incoming is selected.
  ch.addScript(
      "AT+CMGL=\"ALL\"",
      {"+CMGL: 5,\"STO SENT\",\"+70000000005\",\"\",\"26/08/25,20:00:00+12\"", "0041",
       "+CMGL: 2,\"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:01:00+12\"", "0042",
       "+CMGL: 6,\"STO UNSENT\",\"+70000000006\",\"\",\"26/08/25,20:02:00+12\"", "0043", "OK"});
  ch.addScript("AT+CMGR=2", {"+CMGR: \"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:01:00+12\"",
                             "0042", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  expectNoViolations(ch, "ignore_outgoing");
  assert(strcmp(sms.id, "2") == 0);
  puts("testFindOldestUnreadIgnoresOutgoing ok");
}
// #endregion FUNC_testFindOldestUnreadIgnoresOutgoing

// #region FUNC_testReadDeleteSend
// PURPOSE: Covers direct read, delete, and input rejection boundaries.
void testReadDeleteSend() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript(
      "AT+CMGR=1",
      {"+CMGR: \"REC "
       "READ\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
       "25,16:59:23+12\",145,4,0,8,\"002B00370039003600390033003500300031003300350037\",145,5",
       "0421043C0441043A0430", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  assert(client.readSms("1", sms) == ModemResult::kSuccess);
  assert(strcmp(sms.id, "1") == 0);
  // delete
  FakeModemChannel ch2;
  ch2.addScript("AT+CMGD=1", {"OK"});
  ModemClient client2(ch2, scratch, sizeof(scratch));
  assert(client2.deleteSms("1") == ModemResult::kSuccess);
  // send validation (no AT) — shared 335-unit limit via sms_validate.h
  FakeModemChannel ch3;
  ModemClient client3(ch3, scratch, sizeof(scratch));
  assert(client3.sendSms("", "Hello") == ModemResult::kProtocolError);
  assert(strcmp(client3.failedStage(), "send_input") == 0);
  assert(client3.sendSms("+7999", "") == ModemResult::kProtocolError);
  assert(strcmp(client3.failedStage(), "send_input") == 0);
  std::string tooLong(336, 'A');
  assert(client3.sendSms("+7999", tooLong.c_str()) == ModemResult::kProtocolError);
  assert(strcmp(client3.failedStage(), "send_input") == 0);
  assert(client3.sendSms("+7999", "\xFF") == ModemResult::kProtocolError);
  assert(strcmp(client3.failedStage(), "send_input") == 0);
  assert(client3.sendSms("++7999", "Hello") == ModemResult::kProtocolError);
  assert(strcmp(client3.failedStage(), "send_input") == 0);
  puts("testReadDeleteSend ok");
}
// #endregion FUNC_testReadDeleteSend

// #region FUNC_testFindUnreadCandidates
// PURPOSE: Ensures one drained CMGL listing exposes every candidate before CMGR reads.
void testFindUnreadCandidates() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGL=\"ALL\"",
               {"+CMGL: 8,\"STO SENT\",\"+70000000008\",\"\",\"26/08/25,20:00:00+12\"",
                "+CMGL: 7,\"REC READ\",\"+70000000007\",\"\",\"26/08/25,20:00:00+12\"",
                "+CMGL: 2,\"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:00:00+12\"",
                "+CMGL: 5,\"REC UNREAD\",\"+70000000005\",\"\",\"26/08/25,20:00:00+12\"", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemInboxCandidate candidates[3];
  size_t count = 0;
  assert(client.findUnreadCandidates(candidates, 3, count) == ModemResult::kSuccess);
  expectNoViolations(ch, "find_unread_candidates");
  assert(count == 3 && strcmp(candidates[0].id, "2") == 0 && strcmp(candidates[1].id, "5") == 0 &&
         strcmp(candidates[2].id, "7") == 0);
  for (unsigned long timeoutMs : ch.readTimeouts()) assert(timeoutMs == 5000);
  puts("testFindUnreadCandidates ok");
}
// #endregion FUNC_testFindUnreadCandidates

// #region FUNC_testSelectStorageMe
// PURPOSE: Keeps normal polling on the modem's ME storage.
void testSelectStorageMe() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CPMS=\"ME\",\"ME\",\"ME\"",
               {"+CPMS: \"ME\",0,100,\"ME\",0,100,\"ME\",0,100", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult meResult = client.selectReadStorage("ME");
  expectNoViolations(ch, "select_me");
  assert(meResult == ModemResult::kSuccess);
  assert(ch.matchedCommands().size() == 1);
  puts("testSelectStorageMe ok");
}
// #endregion FUNC_testSelectStorageMe

// #region FUNC_testSelectStorageSmReadKeepsMeIncoming
// PURPOSE: Prevents SIM fallback reads from redirecting incoming storage.
void testSelectStorageSmReadKeepsMeIncoming() {
  FakeModemChannel ch;
  char scratch[256];
  // SM is a read-only fallback: only mem1 switches, mem2/mem3 stay ME.
  ch.addScript("AT+CPMS=\"SM\",\"ME\",\"ME\"",
               {"+CPMS: \"SM\",1,30,\"ME\",0,100,\"ME\",0,100", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult smResult = client.selectReadStorage("SM");
  expectNoViolations(ch, "select_sm");
  assert(smResult == ModemResult::kSuccess);
  assert(ch.matchedCommands().size() == 1);
  puts("testSelectStorageSmReadKeepsMeIncoming ok");
}
// #endregion FUNC_testSelectStorageSmReadKeepsMeIncoming

// #region FUNC_testBuildUcs2SubmitPdu
// PURPOSE: Keeps SMS-SUBMIT octets, CMGS length, and input validation byte-exact.
void testBuildUcs2SubmitPdu() {
  char out[320];
  size_t octets = 0;
  // Reference layout verified against TS 23.040 (see Этап 9 notes).
  assert(buildUcs2SubmitPdu("+79990000000", "0041", 0xAB, 2, 1, out, sizeof(out), octets));
  assert(strcmp(out, "0041000B919799000000F0000808050003AB02010041") == 0);
  assert(octets == 21);  // TPDU octets without the SCA byte — value for AT+CMGS
  // No leading '+': TON/NPI drops to 0x81 (unknown type).
  assert(buildUcs2SubmitPdu("799900000", "0041", 1, 1, 1, out, sizeof(out), octets));
  assert(strcmp(out, "004100098197990000F00008080500030101010041") == 0);
  assert(octets == 20);
  // Even digit count: no F padding byte in the address.
  assert(buildUcs2SubmitPdu("7999000000", "0041", 1, 1, 1, out, sizeof(out), octets));
  assert(strcmp(out, "0041000A8197990000000008080500030101010041") == 0);
  // Max part (67 units) must yield the full single-part TPDU size.
  std::string maxPart;
  for (int i = 0; i < 67; ++i) maxPart += "0061";
  assert(buildUcs2SubmitPdu("+79990000000", maxPart.c_str(), 1, 1, 1, out, sizeof(out), octets));
  assert(octets == 153);
  // Invalid inputs never render a PDU.
  assert(!buildUcs2SubmitPdu("+79990000000", "0041", 1, 1, 0, out, sizeof(out), octets));
  assert(!buildUcs2SubmitPdu("+79990000000", "0041", 1, 2, 3, out, sizeof(out), octets));
  std::string tooLong;
  for (int i = 0; i < 68; ++i) tooLong += "0061";
  assert(!buildUcs2SubmitPdu("+79990000000", tooLong.c_str(), 1, 1, 1, out, sizeof(out), octets));
  assert(!buildUcs2SubmitPdu("+79990000000", "0041", 1, 1, 1, out, 5, octets));
  assert(!buildUcs2SubmitPdu("+79990000000", nullptr, 1, 1, 1, out, sizeof(out), octets));
  puts("testBuildUcs2SubmitPdu ok");
}
// #endregion FUNC_testBuildUcs2SubmitPdu

// #region FUNC_testModemSendGsmSafeAscii
// PURPOSE: Confirms GSM-safe text uses the direct text-mode payload path.
void testModemSendGsmSafeAscii() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"GSM\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,0", {"OK"});
  ch.addScript("AT+CMGS=\"+79990000000\"", {">", "+CMGS: 123", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  // A prior failed attempt must not mislabel the next send: the best-effort
  // sync and the successful dialog leave no stale stage behind.
  assert(client.sendSms("++7999", "Hello") == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "send_input") == 0);
  const ModemResult gsmResult = client.sendSms("+79990000000", "Hello 123!?");
  expectNoViolations(ch, "gsm_safe_ascii");
  assert(gsmResult == ModemResult::kSuccess);
  assert(strcmp(client.failedStage(), "") == 0);
  assert(ch.dataPayload() == std::string("Hello 123!?\x1A"));
  const std::vector<std::string>& seq = ch.matchedCommands();
  assert(seq.size() == 5);
  assert(seq[0] == "AT");
  assert(seq[1] == "AT+CMGF=1");
  assert(seq[2] == "AT+CSCS=\"GSM\"");
  assert(seq[3] == "AT+CSMP=17,167,0,0");
  assert(seq[4] == "AT+CMGS=\"+79990000000\"");
  puts("testModemSendGsmSafeAscii ok");
}
// #endregion FUNC_testModemSendGsmSafeAscii

// #region FUNC_testModemSendUnsafeAsciiUcs2
// PURPOSE: Confirms GSM-mismatched punctuation takes the UCS-2 path.
void testModemSendUnsafeAsciiUcs2() {
  FakeModemChannel ch;
  char scratch[1024];
  // @$[]_~ exist in ASCII but not at the same GSM 03.38 codes: must take
  // the UCS2 path with the exact hex payload.
  const char* text = "a@$[]_~b";
  char hexNumber[128];
  char hexBody[128];
  codec::encodeUcs2Hex("+79990000000", hexNumber, sizeof(hexNumber));
  codec::encodeUcs2Hex(text, hexBody, sizeof(hexBody));
  char cmgsCmd[160];
  snprintf(cmgsCmd, sizeof(cmgsCmd), "AT+CMGS=\"%s\"", hexNumber);
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,8", {"OK"});
  ch.addScript(cmgsCmd, {">", "+CMGS: 7", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult ucs2Result = client.sendSms("+79990000000", text);
  expectNoViolations(ch, "unsafe_ascii_ucs2");
  assert(ucs2Result == ModemResult::kSuccess);
  assert(ch.dataPayload() == std::string(hexBody) + "\x1A");
  puts("testModemSendUnsafeAsciiUcs2 ok");
}
// #endregion FUNC_testModemSendUnsafeAsciiUcs2

// #region FUNC_testModemSendCyrillic
// PURPOSE: Confirms UTF-8 Cyrillic becomes the expected UCS-2 payload.
void testModemSendCyrillic() {
  FakeModemChannel ch;
  char scratch[1024];
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,8", {"OK"});
  // "Привет" -> 041F04400438043204350442
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"",
               {">", "+CMGS: 45", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const char* text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
  const ModemResult cyrResult = client.sendSms("+79990000000", text);
  expectNoViolations(ch, "send_cyrillic");
  assert(cyrResult == ModemResult::kSuccess);
  char hexBody[128];
  codec::encodeUcs2Hex(text, hexBody, sizeof(hexBody));
  assert(ch.dataPayload() == std::string(hexBody) + "\x1A");
  puts("testModemSendCyrillic ok");
}
// #endregion FUNC_testModemSendCyrillic

// #region FUNC_testIsDirectGsmAsciiText
// PURPOSE: Defines the safe ASCII subset that can be sent as GSM text.
void testIsDirectGsmAsciiText() {
  assert(isDirectGsmAsciiText(""));
  assert(isDirectGsmAsciiText("Hello 123!?"));
  assert(isDirectGsmAsciiText(" !\"#"));
  assert(isDirectGsmAsciiText("%&'()*+,-./0123456789:;<=>?"));
  assert(isDirectGsmAsciiText("AZaz"));
  // GSM-mismatched ASCII punctuation must fall back to UCS2.
  assert(!isDirectGsmAsciiText("a@$[]_~b"));
  assert(!isDirectGsmAsciiText("$"));
  assert(!isDirectGsmAsciiText("@"));
  assert(!isDirectGsmAsciiText("[\\]^_"));
  assert(!isDirectGsmAsciiText("`"));
  assert(!isDirectGsmAsciiText("{|}~"));
  // Control characters and non-ASCII UTF-8 never go raw.
  assert(!isDirectGsmAsciiText("a\nb"));
  assert(!isDirectGsmAsciiText("a\x7F"));
  assert(!isDirectGsmAsciiText("\xD0\x9F"));
  assert(!isDirectGsmAsciiText(nullptr));
  puts("testIsDirectGsmAsciiText ok");
}
// #endregion FUNC_testIsDirectGsmAsciiText

// #region FUNC_testModemSendPromptTimeout
// PURPOSE: Ensures a missing CMGS prompt reports a bounded timeout.
void testModemSendPromptTimeout() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"GSM\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,0", {"OK"});
  // No '>' prompt -> timeout (empty script returns -1)
  ch.addScript("AT+CMGS=\"+79990000000\"", {});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kTimeout);
  assert(strcmp(client.failedStage(), "cmgs_prompt") == 0);
  puts("testModemSendPromptTimeout ok");
}
// #endregion FUNC_testModemSendPromptTimeout

// #region FUNC_testModemSendCmsError
// PURPOSE: Maps a modem CMS error to the stable send rejection result.
void testModemSendCmsError() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"GSM\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,0", {"OK"});
  ch.addScript("AT+CMGS=\"+79990000000\"", {">", "+CMS ERROR: 500", "ERROR"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kSendRejected);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  puts("testModemSendCmsError ok");
}
// #endregion FUNC_testModemSendCmsError

// #region FUNC_testModemSendProtocolError
// PURPOSE: Rejects a CMGS reply that lacks its success reference.
void testModemSendProtocolError() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"GSM\"", {"OK"});
  ch.addScript("AT+CSMP=17,167,0,0", {"OK"});
  // OK without +CMGS -> protocol error
  ch.addScript("AT+CMGS=\"+79990000000\"", {">", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "protocol") == 0);
  puts("testModemSendProtocolError ok");
}
// #endregion FUNC_testModemSendProtocolError

// #region FUNC_testModemSendMultipartTwoParts
// PURPOSE: Pins two-part UCS-2 concatenation boundaries and payloads.
void testModemSendMultipartTwoParts() {
  FakeModemChannel ch;
  char scratch[1024];
  // 80 Cyrillic units exceed the 70-unit single UCS2 segment: text mode
  // would truncate or reject, so the text must go out as 2 concat parts.
  std::string text;
  for (int i = 0; i < 80; ++i) text += "\xD0\x9F";  // 'П' -> UCS2 041F
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=0", {"OK"});
  ch.addScript("AT+CMGS=153", {">", "+CMGS: 1", "OK"});
  ch.addScript("AT+CMGS=45", {">", "+CMGS: 2", "OK"});
  ch.addScript("AT+CMGF=1", {"OK"});  // text mode restored after success
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult twoPartsResult = client.sendSms("+79990000000", text.c_str());
  expectNoViolations(ch, "multipart_two_parts");
  assert(twoPartsResult == ModemResult::kSuccess);
  std::string units67;
  std::string units13;
  for (int i = 0; i < 67; ++i) units67 += "041F";
  for (int i = 0; i < 13; ++i) units13 += "041F";
  assert(ch.matchedCommands().size() == 5);
  assert(ch.matchedCommands()[1] == "AT+CMGF=0");
  assert(ch.matchedCommands()[4] == "AT+CMGF=1");
  // UDL 8C/20, UDH 05 0003 01 02 01/02: ref 1, total 2, seq 1 and 2.
  assert(fakePayloadPart(ch, 0) == "0041000B919799000000F000088C050003010201" + units67 + "\x1A");
  assert(fakePayloadPart(ch, 1) == "0041000B919799000000F0000820050003010202" + units13 + "\x1A");
  puts("testModemSendMultipartTwoParts ok");
}
// #endregion FUNC_testModemSendMultipartTwoParts

// #region FUNC_testModemSendMultipartThreeParts
// PURPOSE: Pins three-part fallback when GSM text exceeds one segment.
void testModemSendMultipartThreeParts() {
  FakeModemChannel ch;
  char scratch[1024];
  // 161 plain 'a' is GSM-safe but past the 160-char single segment: the
  // overflow must go out as UCS2 concat parts, not be cut to 160 chars.
  const std::string text(161, 'a');
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=0", {"OK"});
  ch.addScript("AT+CMGS=153", {">", "+CMGS: 1", "OK"});
  ch.addScript("AT+CMGS=153", {">", "+CMGS: 2", "OK"});
  ch.addScript("AT+CMGS=73", {">", "+CMGS: 3", "OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult threePartsResult = client.sendSms("+79990000000", text.c_str());
  expectNoViolations(ch, "multipart_three_parts");
  assert(threePartsResult == ModemResult::kSuccess);
  std::string units67;
  std::string units27;
  for (int i = 0; i < 67; ++i) units67 += "0061";
  for (int i = 0; i < 27; ++i) units27 += "0061";
  assert(fakePayloadPart(ch, 0) == "0041000B919799000000F000088C050003010301" + units67 + "\x1A");
  assert(fakePayloadPart(ch, 1) == "0041000B919799000000F000088C050003010302" + units67 + "\x1A");
  assert(fakePayloadPart(ch, 2) == "0041000B919799000000F000083C050003010303" + units27 + "\x1A");
  puts("testModemSendMultipartThreeParts ok");
}
// #endregion FUNC_testModemSendMultipartThreeParts

// #region FUNC_testModemSendMultipartPart2Fails
// PURPOSE: Ensures multipart failure still restores text mode.
void testModemSendMultipartPart2Fails() {
  FakeModemChannel ch;
  char scratch[1024];
  std::string text;
  for (int i = 0; i < 80; ++i) text += "\xD0\x9F";
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=0", {"OK"});
  ch.addScript("AT+CMGS=153", {">", "+CMGS: 1", "OK"});
  ch.addScript("AT+CMGS=45", {">", "+CMS ERROR: 500"});
  ch.addScript("AT+CMGF=1", {"OK"});  // restore must run after the failure too
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult failResult = client.sendSms("+79990000000", text.c_str());
  expectNoViolations(ch, "multipart_part2_fails");
  assert(failResult == ModemResult::kSendRejected);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  assert(fakeSentCommand(ch, "AT+CMGF=1"));
  puts("testModemSendMultipartPart2Fails ok");
}
// #endregion FUNC_testModemSendMultipartPart2Fails

// #region FUNC_testModemSendSurrogateNotSplit
// PURPOSE: Keeps UTF-16 surrogate pairs intact across multipart boundaries.
void testModemSendSurrogateNotSplit() {
  FakeModemChannel ch;
  char scratch[1024];
  // 66 'a' + one astral emoji (2 UTF-16 units) + 10 'b' = 78 units. The
  // natural 67-unit boundary would cut the surrogate pair in half; the
  // boundary must shift back so the pair travels inside part 2.
  std::string text(66, 'a');
  text += "\xF0\x9F\x98\xBA";  // U+1F63A -> D83D DE3A
  text += std::string(10, 'b');
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=0", {"OK"});
  ch.addScript("AT+CMGS=151", {">", "+CMGS: 1", "OK"});
  ch.addScript("AT+CMGS=43", {">", "+CMGS: 2", "OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult surrogateResult = client.sendSms("+79990000000", text.c_str());
  expectNoViolations(ch, "multipart_surrogate");
  assert(surrogateResult == ModemResult::kSuccess);
  std::string units66;
  std::string units10;
  for (int i = 0; i < 66; ++i) units66 += "0061";
  for (int i = 0; i < 10; ++i) units10 += "0062";
  assert(fakePayloadPart(ch, 0) == "0041000B919799000000F000088A050003010201" + units66 + "\x1A");
  assert(fakePayloadPart(ch, 1) ==
         std::string("0041000B919799000000F000081E050003010202") + "D83DDE3A" + units10 + "\x1A");
  puts("testModemSendSurrogateNotSplit ok");
}
// #endregion FUNC_testModemSendSurrogateNotSplit

// #region FUNC_testModemSendSegmentBoundaries
// PURPOSE: Preserves single-part limits for GSM and UCS-2 text.
void testModemSendSegmentBoundaries() {
  // 160 'a' exactly fills the single GSM segment: the proven text-mode path
  // must stay (no PDU switch).
  {
    FakeModemChannel ch;
    char scratch[512];
    ch.addScript("AT", {"OK"});
    ch.addScript("AT+CMGF=1", {"OK"});
    ch.addScript("AT+CSCS=\"GSM\"", {"OK"});
    ch.addScript("AT+CSMP=17,167,0,0", {"OK"});
    ch.addScript("AT+CMGS=\"+79990000000\"", {">", "+CMGS: 1", "OK"});
    ModemClient client(ch, scratch, sizeof(scratch));
    const ModemResult gsm160 = client.sendSms("+79990000000", std::string(160, 'a').c_str());
    expectNoViolations(ch, "boundary_gsm_160");
    assert(gsm160 == ModemResult::kSuccess);
    assert(ch.dataPayload() == std::string(160, 'a') + "\x1A");
    assert(!fakeSentCommand(ch, "AT+CMGF=0"));
  }
  // 70 Cyrillic units exactly fill the single UCS2 segment: text mode stays.
  {
    FakeModemChannel ch;
    char scratch[1024];
    std::string text;
    const std::string cyrUnit = "\xD0\x9F";
    for (int i = 0; i < 70; ++i) text += cyrUnit;  // exactly 70 UTF-16 units
    char hexNumber[128];
    codec::encodeUcs2Hex("+79990000000", hexNumber, sizeof(hexNumber));
    char cmgsCmd[160];
    snprintf(cmgsCmd, sizeof(cmgsCmd), "AT+CMGS=\"%s\"", hexNumber);
    ch.addScript("AT", {"OK"});
    ch.addScript("AT+CMGF=1", {"OK"});
    ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
    ch.addScript("AT+CSMP=17,167,0,8", {"OK"});
    ch.addScript(cmgsCmd, {">", "+CMGS: 2", "OK"});
    ModemClient client(ch, scratch, sizeof(scratch));
    const ModemResult ucs270 = client.sendSms("+79990000000", text.c_str());
    expectNoViolations(ch, "boundary_ucs2_70");
    assert(ucs270 == ModemResult::kSuccess);
    assert(!fakeSentCommand(ch, "AT+CMGF=0"));
  }
  puts("testModemSendSegmentBoundaries ok");
}
// #endregion FUNC_testModemSendSegmentBoundaries

// #region FUNC_testModemSendMaxUnitsMultipart
// PURPOSE: Preserves the shared maximum SMS length without truncation.
void testModemSendMaxUnitsMultipart() {
  FakeModemChannel ch;
  char scratch[2048];
  // 335 units == kMaxSmsSendUnits: the whole shared-cap range must reach the
  // peer as exactly 5 concat parts of 67 units (no truncation, no 6th part).
  std::string text(334, 'a');
  text += '@';  // unsafe byte forces the UCS2 path at max length
  char hexBody[1400];
  assert(codec::encodeUcs2Hex(text.c_str(), hexBody, sizeof(hexBody)) == 1340);
  ch.addScript("AT", {"OK"});
  ch.addScript("AT+CMGF=0", {"OK"});
  for (int part = 1; part <= 5; ++part) ch.addScript("AT+CMGS=153", {">", "+CMGS: 1", "OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  const ModemResult maxResult = client.sendSms("+79990000000", text.c_str());
  expectNoViolations(ch, "max_units_multipart");
  assert(maxResult == ModemResult::kSuccess);
  std::string units67;
  for (int i = 0; i < 67; ++i) units67 += "0061";
  std::string units66;
  for (int i = 0; i < 66; ++i) units66 += "0061";
  for (int part = 1; part <= 5; ++part) {
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "0041000B919799000000F000088C05000301050%d", part);
    std::string expect = prefix + units67 + "\x1A";
    if (part == 5) expect = prefix + units66 + "0040\x1A";  // last unit is '@'
    assert(fakePayloadPart(ch, static_cast<size_t>(part - 1)) == expect);
  }
  puts("testModemSendMaxUnitsMultipart ok");
}
// #endregion FUNC_testModemSendMaxUnitsMultipart

// #region FUNC_testModemSendAstralMaxUnitsMultipart
// PURPOSE: Ensures astral codepoints produce legal non-splitting PDU parts.
void testModemSendAstralMaxUnitsMultipart() {
  // 167 astral code points occupy 334 UTF-16 units. A 67-unit part boundary
  // would split every pair, so six legal PDU parts are required.
  const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600
  std::string units66;
  for (int i = 0; i < 33; ++i) units66 += "D83DDE00";
  std::string units4 = "D83DDE00D83DDE00";
  for (const bool addBmp : {false, true}) {
    FakeModemChannel ch;
    char scratch[2048];
    std::string text;
    for (int i = 0; i < 167; ++i) text += emoji;
    if (addBmp) text += 'A';
    ch.addScript("AT", {"OK"});
    ch.addScript("AT+CMGF=0", {"OK"});
    for (int part = 1; part <= 5; ++part) ch.addScript("AT+CMGS=151", {">", "+CMGS: 1", "OK"});
    ch.addScript(addBmp ? "AT+CMGS=29" : "AT+CMGS=27", {">", "+CMGS: 1", "OK"});
    ch.addScript("AT+CMGF=1", {"OK"});
    ModemClient client(ch, scratch, sizeof(scratch));
    assert(client.sendSms("+79990000000", text.c_str()) == ModemResult::kSuccess);
    expectNoViolations(ch, addBmp ? "astral_335_units" : "astral_334_units");
    assert(ch.matchedCommands().size() == 9);
    for (int part = 1; part <= 5; ++part) {
      char prefix[48];
      snprintf(prefix, sizeof(prefix), "0041000B919799000000F000088A05000301060%d", part);
      assert(fakePayloadPart(ch, static_cast<size_t>(part - 1)) ==
             std::string(prefix) + units66 + "\x1A");
    }
    const std::string last = addBmp ? "D83DDE00D83DDE000041" : units4;
    assert(fakePayloadPart(ch, 5) == std::string(addBmp
                                                     ? "0041000B919799000000F0000810050003010606"
                                                     : "0041000B919799000000F000080E050003010606") +
                                         last + "\x1A");
  }
  puts("testModemSendAstralMaxUnitsMultipart ok");
}
// #endregion FUNC_testModemSendAstralMaxUnitsMultipart

// #region FUNC_testInitSuccess
// PURPOSE: Pins the successful modem initialization command sequence.
void testInitSuccess() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT", {"OK"});
  ch.addScript("ATE0", {"OK"});
  ch.addScript("ATV1", {"OK"});
  ch.addScript("AT+CMEE=2", {"OK"});
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  ch.addScript("AT+CSDH=1", {"OK"});
  ch.addScript("AT+CPMS=\"ME\",\"ME\",\"ME\"", {"OK"});
  ch.addScript("AT+CNMI=2,1,0,0,0", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.init() == ModemResult::kSuccess);
  puts("testInitSuccess ok");
}
// #endregion FUNC_testInitSuccess

// #region FUNC_testInitNotPresent
// PURPOSE: Ensures an unresponsive modem reports absence cleanly.
void testInitNotPresent() {
  class TimeoutChannel : public ModemChannel {
   public:
    bool write(const char*, size_t) override { return true; }
    int readLine(char*, size_t, unsigned long) override { return -1; }
    void purge() override {}
  } ch;
  char scratch[256];
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.init() == ModemResult::kNotPresent);
  assert(strcmp(client.failedStage(), "not_present") == 0);
  puts("testInitNotPresent ok");
}
// #endregion FUNC_testInitNotPresent

// #region FUNC_testDeleteCmsError
// PURPOSE: Maps delete-time CMS errors to the stable protocol result.
void testDeleteCmsError() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGD=1", {"+CMS ERROR: 500"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.deleteSms("1") == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  puts("testDeleteCmsError ok");
}
// #endregion FUNC_testDeleteCmsError

// #region FUNC_testReadCmsError
// PURPOSE: Maps read-time CMS errors to the stable protocol result.
void testReadCmsError() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGR=2", {"+CMS ERROR: 321"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  assert(client.readSms("2", sms) == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  puts("testReadCmsError ok");
}
// #endregion FUNC_testReadCmsError

// #region FUNC_testReadMissingOk
// PURPOSE: Rejects a complete-looking CMGR payload when its terminal OK is
// absent, so an incomplete modem reply cannot be forwarded or deleted.
void testReadMissingOk() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGR=2",
               {"+CMGR: \"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:00:00+12\"", "0041",
                "+CMTI: \"ME\",1", "+CMTI: \"ME\",2", "+CMTI: \"ME\",3", "+CMTI: \"ME\",4",
                "+CMTI: \"ME\",5", "+CMTI: \"ME\",6", "+CMTI: \"ME\",7", "+CMTI: \"ME\",8"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  assert(client.readSms("2", sms) == ModemResult::kTimeout);
  assert(strcmp(client.failedStage(), "cmgr_no_ok") == 0);
  expectNoViolations(ch, "read_missing_ok");
  puts("testReadMissingOk ok");
}
// #endregion FUNC_testReadMissingOk

int main() {
  testParseCpin();
  testParseCsq();
  testParseCesq();
  testParseCreg();
  testNetworkRegistrationGate();
  testParseCops();
  testParseCpms();
  testParseCclk();
  testParseImei();
  testParseFw();
  testPollStatus();
  testModemRecordValidation();
  testCodecUcs2();
  testParseCmglHeader();
  testParseCmgrHeader();
  testParseCmglEntry();
  testParseCmgrEntry();
  testParsePduConcat();
  testExtractConcatFromDeliverPdu();
  testProbeConcat();
  testModemConcatCache();
  testFindOldestUnreadUcs2();
  testFindOldestUnreadGsm();
  testFindOldestUnreadOrdered();
  testFindOldestUnreadEmptyAndError();
  testFindOldestUnreadFullStorage();
  testFindOldestUnreadMissingOk();
  testFindOldestUnreadRecRead();
  testFindOldestUnreadIgnoresOutgoing();
  testFindUnreadCandidates();
  testSelectStorageMe();
  testSelectStorageSmReadKeepsMeIncoming();
  testReadDeleteSend();
  testModemSendGsmSafeAscii();
  testModemSendUnsafeAsciiUcs2();
  testModemSendCyrillic();
  testIsDirectGsmAsciiText();
  testBuildUcs2SubmitPdu();
  testModemSendSegmentBoundaries();
  testModemSendMultipartTwoParts();
  testModemSendMultipartThreeParts();
  testModemSendMultipartPart2Fails();
  testModemSendSurrogateNotSplit();
  testModemSendMaxUnitsMultipart();
  testModemSendAstralMaxUnitsMultipart();
  testModemSendPromptTimeout();
  testModemSendCmsError();
  testModemSendProtocolError();
  testInitSuccess();
  testInitNotPresent();
  testDeleteCmsError();
  testReadCmsError();
  testReadMissingOk();
  puts("all modem tests passed");
  return 0;
}
