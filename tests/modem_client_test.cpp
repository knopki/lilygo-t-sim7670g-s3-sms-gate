// #region MODULE_CONTRACT
// PURPOSE: Host tests for SIM7670G modem_client parsers and pollStatus
// sequencing (ADR-0004). Uses FakeModemChannel to script AT replies without
// hardware; mirrors zte_client_test.cpp style.
// #endregion MODULE_CONTRACT

#include "../sms_gate/modem_client.h"
#include <cassert>
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
  return 0;
}
