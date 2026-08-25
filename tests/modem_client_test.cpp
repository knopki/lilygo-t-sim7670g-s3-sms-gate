// #region MODULE_CONTRACT
// PURPOSE: Host tests for SIM7670G modem_client parsers and pollStatus
// sequencing (ADR-0004). Uses FakeModemChannel to script AT replies without
// hardware; mirrors zte_client_test.cpp style.
// #endregion MODULE_CONTRACT

#include "../sms_gate/codec.h"
#include "../sms_gate/modem_client.h"
#include "../sms_gate/modem_record.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// #region CLASS_FakeModemChannel
// PURPOSE: Scripts AT request → reply lines for deterministic parser tests.
class FakeModemChannel : public ModemChannel {
 public:
  struct Script {
    std::string expectCmd;
    std::vector<std::string> replyLines;
  };
  std::vector<Script> scripts;
  size_t scriptIdx = 0;
  size_t lineIdx = 0;
  std::string lastWrite;

  void addScript(const std::string& cmd, std::vector<std::string> lines) {
    scripts.push_back({cmd, lines});
  }

  bool write(const char* data, size_t len) override {
    lastWrite.assign(data, len);
    // Trim CRLF for matching
    std::string trimmed = lastWrite;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n'))
      trimmed.pop_back();
    // Advance to matching script if needed (allow any order for simplicity)
    if (scriptIdx < scripts.size() && scripts[scriptIdx].expectCmd == trimmed) {
      lineIdx = 0;
    }
    return true;
  }
  int readLine(char* buffer, size_t size, unsigned long) override {
    if (scriptIdx >= scripts.size()) return -1;
    auto& sc = scripts[scriptIdx];
    if (lineIdx >= sc.replyLines.size()) {
      // Move to next script on next write; for now signal end
      return -1;
    }
    std::string line = sc.replyLines[lineIdx++];
    if (lineIdx >= sc.replyLines.size()) {
      scriptIdx++;
    }
    if (line.size() + 1 > size) return -1;
    memcpy(buffer, line.c_str(), line.size() + 1);
    return (int)line.size();
  }
  void purge() override {}

  void reset() {
    scriptIdx = 0;
    lineIdx = 0;
    lastWrite.clear();
  }
};
// #endregion CLASS_FakeModemChannel

// #region FUNC_testParseCpin
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
void testParseCsq() {
  int rssi, ber;
  assert(parseCsqLine("+CSQ: 22,0", rssi, ber) && rssi == 22 && ber == 0);
  assert(parseCsqLine("+CSQ: 99,99", rssi, ber) && rssi == 99);
  assert(!parseCsqLine("+CSQ: ", rssi, ber));
}
// #endregion FUNC_testParseCsq

// #region FUNC_testParseCesq
void testParseCesq() {
  int rsrp, rsrq;
  assert(parseCesqLine("+CESQ: 99,99,255,255,30,60", rsrp, rsrq));
  assert(rsrp == -80);  // -140+60
  assert(parseCesqLine("+CESQ: 99,99,255,255,255,255", rsrp, rsrq) && rsrp == 0);
}
// #endregion FUNC_testParseCesq

// #region FUNC_testParseCreg
void testParseCreg() {
  int stat;
  assert(parseCregLine("+CEREG: 0,1", stat) && stat == 1);
  assert(parseCregLine("+CREG: 0,5", stat) && stat == 5);
  assert(parseCregLine("+CEREG: 0,6", stat) && stat == 6);
}
// #endregion FUNC_testParseCreg

// #region FUNC_testParseCops
void testParseCops() {
  char op[32];
  int act;
  assert(parseCopsLine("+COPS: 0,2,\"25020\",7", op, sizeof(op), act) && strcmp(op, "25020") == 0 &&
         act == 7);
  assert(parseCopsLine("+COPS: 0", op, sizeof(op), act));
}
// #endregion FUNC_testParseCops

// #region FUNC_testParseCpms
void testParseCpms() {
  uint16_t used, total;
  assert(parseCpmsLine("+CPMS: \"ME\",3,100,\"ME\",3,100,\"ME\",3,100", used, total) && used == 3 &&
         total == 100);
  assert(parseCpmsLine("+CPMS: \"SM\",0,30,\"SM\",0,30,\"SM\",0,30", used, total) && total == 30);
}
// #endregion FUNC_testParseCpms

// #region FUNC_testParseCclk
void testParseCclk() {
  char out[32];
  assert(parseCclkLine("+CCLK: \"25/08/25,12:34:56+12\"", out, sizeof(out)) &&
         strcmp(out, "25/08/25,12:34:56+12") == 0);
}
// #endregion FUNC_testParseCclk

// #region FUNC_testParseImei
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
void testParseFw() {
  char out[48];
  assert(parseFwLine("2374B03SIM767XM5A_M", out, sizeof(out)) &&
         strcmp(out, "2374B03SIM767XM5A_M") == 0);
  assert(parseFwLine("+CGMR: 2374B03SIM767XM5A_M", out, sizeof(out)) &&
         strcmp(out, "2374B03SIM767XM5A_M") == 0);
}
// #endregion FUNC_testParseFw

// #region FUNC_makeModemSourceRecord
// PURPOSE: Builds one known-good modem-source record as the baseline for
// every mutation.
ModemSourceRecord makeModemRecord() {
  ModemSourceRecord record{};
  record.magic = kModemSourceMagic;
  record.version = kModemSourceVersion;
  record.enabled = 1;
  record.pollIntervalSec = kDefaultModemPollSec;
  strcpy(record.label, "+79990000001");
  record.checksum = calculateModemSourceChecksum(record);
  return record;
}
// #endregion FUNC_makeModemSourceRecord

// #region FUNC_testModemRecordValidation
// PURPOSE: Gates load/save on the shared predicate: corrupt, foreign,
// non-printable, and out-of-range intervals never reach the poll path;
// the label is optional but must be printable when present.
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
  record.enabled = 7;
  record.checksum = calculateModemSourceChecksum(record);
  assert(!isModemSourceRecordValid(record));

  record = makeModemRecord();
  record.enabled = 0;
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

  puts("testModemRecordValidation ok");
}
// #endregion FUNC_testModemRecordValidation

// #region FUNC_testPollStatus
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
  ch.addScript("AT+CPMS=\"SM\",\"SM\",\"SM\"", {"OK"});
  ch.addScript("AT+CPMS?", {"+CPMS: \"SM\",1,30,\"SM\",1,30,\"SM\",1,30", "OK"});
  ch.addScript("AT+CPMS=\"ME\",\"ME\",\"ME\"", {"OK"});
  ch.addScript("AT+CCLK?", {"+CCLK: \"25/08/25,12:34:56+12\"", "OK"});
  ch.addScript("AT+CGSN", {"864567789012345", "OK"});
  ch.addScript("AT+CGMR", {"+CGMR: 2374B03SIM767XM5A_M", "OK"});

  ModemClient client(ch, scratch, sizeof(scratch));
  ModemStatus st;
  ModemResult r = client.pollStatus(st);
  assert(r == ModemResult::kSuccess);
  assert(st.present);
  assert(strcmp(st.cpin, "READY") == 0);
  assert(st.csqRssi == 22);
  assert(st.ceregStat == 1);
  assert(st.cgatt);
  assert(strcmp(st.copsOp, "25020") == 0);
  assert(st.smsUsedMe == 3 && st.smsTotalMe == 100);
  assert(st.smsUsedSm == 1);
  assert(strcmp(st.imei, "864567789012345") == 0);
  assert(strcmp(st.fw, "2374B03SIM767XM5A_M") == 0);
}
// #endregion FUNC_testPollStatus

// #region FUNC_testCodecUcs2
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
void testParsePduConcat() {
  uint8_t ref = 0, total = 0, seq = 0;
  assert(parsePduConcat(
      "07919796531053F7440B919786557561F10008628052021385218C050003530401041A043804400438043B", ref,
      total, seq));
  assert(ref == 0x53 && total == 4 && seq == 1);
  assert(parsePduConcat("050003530403043B", ref, total, seq) && seq == 3);
  assert(!parsePduConcat("00480065006C", ref, total, seq));
  assert(!parsePduConcat("", ref, total, seq));
  // lower-case hex search
  assert(parsePduConcat("050003ab0201", ref, total, seq));
  assert(ref == 0xAB && total == 2 && seq == 1);
  puts("testParsePduConcat ok");
}
// #endregion FUNC_testParsePduConcat

// #region FUNC_testFindOldestUnreadScenarios
void testFindOldestUnreadUcs2() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGL=\"REC UNREAD\"",
               {"+CMGL: 1,\"REC "
                "UNREAD\",\"002B00370039003600380035003500350037003100360031\",\"\",\"26/08/"
                "25,20:29:40+12\"",
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
void testFindOldestUnreadGsm() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGL=\"REC UNREAD\"",
               {"+CMGL: 0,\"REC UNREAD\",\"+79685557161\",\"\",\"26/08/25,20:31:14+12\"",
                "Hello test 123", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  assert(strcmp(sms.text, "Hello test 123") == 0);
  puts("testFindOldestUnreadGsm ok");
}
void testFindOldestUnreadOrdered() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript(
      "AT+CMGL=\"REC UNREAD\"",
      {"+CMGL: 5,\"REC UNREAD\",\"+70000000001\",\"\",\"26/08/25,20:31:58+12\",145,4", "0054",
       "+CMGL: 2,\"REC UNREAD\",\"+70000000002\",\"\",\"26/08/25,20:29:40+12\",145,4", "0041",
       "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = false;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && found);
  assert(strcmp(sms.id, "2") == 0);
  assert(strcmp(sms.text, "A") == 0);
  assert(sms.concatComplete);
  puts("testFindOldestUnreadOrdered ok");
}
void testFindOldestUnreadEmptyAndError() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGL=\"REC UNREAD\"", {"OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  ModemSms sms{};
  bool found = true;
  assert(client.findOldestUnread(sms, found) == ModemResult::kSuccess && !found);
  // CMS ERROR path
  FakeModemChannel ch2;
  ch2.addScript("AT+CMGL=\"REC UNREAD\"", {"+CMS ERROR: 500"});
  ModemClient client2(ch2, scratch, sizeof(scratch));
  assert(client2.findOldestUnread(sms, found) == ModemResult::kProtocolError);
  assert(strcmp(client2.failedStage(), "cms_error") == 0);
  puts("testFindOldestUnreadEmptyAndError ok");
}
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
// #endregion FUNC_testFindOldestUnreadScenarios

// #region FUNC_testModemSend
void testModemSendUcs2() {
  FakeModemChannel ch;
  char scratch[512];
  // Number +79990000000 -> 002B00370039003900390030003000300030003000300030, "Hello" -> 00480065006C006C006F
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"", {">", "+CMGS: 123", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hello") == ModemResult::kSuccess);
  puts("testModemSendUcs2 ok");
}
void testModemSendCyrillic() {
  FakeModemChannel ch;
  char scratch[1024];
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  // "Привет" -> 041F04400438043204350442
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"", {">", "+CMGS: 45", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == ModemResult::kSuccess);
  puts("testModemSendCyrillic ok");
}
void testModemSendPromptTimeout() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  // No '>' prompt -> timeout (empty script returns -1)
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"", {});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kTimeout);
  assert(strcmp(client.failedStage(), "cmgs_prompt") == 0);
  puts("testModemSendPromptTimeout ok");
}
void testModemSendCmsError() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"", {">", "+CMS ERROR: 500", "ERROR"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kSendRejected);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  puts("testModemSendCmsError ok");
}
void testModemSendProtocolError() {
  FakeModemChannel ch;
  char scratch[512];
  ch.addScript("AT+CMGF=1", {"OK"});
  ch.addScript("AT+CSCS=\"UCS2\"", {"OK"});
  // OK without +CMGS -> protocol error
  ch.addScript("AT+CMGS=\"002B00370039003900390030003000300030003000300030\"", {">", "OK"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.sendSms("+79990000000", "Hi") == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "protocol") == 0);
  puts("testModemSendProtocolError ok");
}
// #endregion FUNC_testModemSend

// #region FUNC_testInitSequence
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
void testDeleteCmsError() {
  FakeModemChannel ch;
  char scratch[256];
  ch.addScript("AT+CMGD=1", {"+CMS ERROR: 500"});
  ModemClient client(ch, scratch, sizeof(scratch));
  assert(client.deleteSms("1") == ModemResult::kProtocolError);
  assert(strcmp(client.failedStage(), "cms_error") == 0);
  puts("testDeleteCmsError ok");
}
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
// #endregion FUNC_testInitSequence

int main() {
  testParseCpin();
  testParseCsq();
  testParseCesq();
  testParseCreg();
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
  testFindOldestUnreadUcs2();
  testFindOldestUnreadGsm();
  testFindOldestUnreadOrdered();
  testFindOldestUnreadEmptyAndError();
  testReadDeleteSend();
  testModemSendUcs2();
  testModemSendCyrillic();
  testModemSendPromptTimeout();
  testModemSendCmsError();
  testModemSendProtocolError();
  testInitSuccess();
  testInitNotPresent();
  testDeleteCmsError();
  testReadCmsError();
  puts("all modem tests passed");
  return 0;
}
