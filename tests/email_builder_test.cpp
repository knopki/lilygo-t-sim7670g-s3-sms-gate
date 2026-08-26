// #region MODULE_CONTRACT
// PURPOSE: Host test for email_builder — sanitizeSenderForSubject and
// buildSmsEmailFromParts / buildZteSmsEmail / buildModemSmsEmail. Asserts
// printable-ASCII, 40-char cap, unknown sender, INCOMPLETE flag and body
// Received-on alias.
// #endregion MODULE_CONTRACT
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../sms_gate/system/email_builder.h"
#include "../sms_gate/modem/modem_client.h"
#include "../sms_gate/zte/zte_client.h"

// Stub formatZteDate — real impl lives in zte_client.cpp, avoid pulling channel logic.
bool formatZteDate(const char* raw, char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  if (!raw) raw = "";
  // mimic fallback: copy raw truncated
  strncpy(out, raw, outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

static int run = 0, pass = 0;
#define EXPECT(cond, msg) do { ++run; if (cond) { ++pass; } else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while(0)

static bool contains(const String& h, const char* needle) {
  return std::string(h.c_str()).find(needle) != std::string::npos;
}

int main() {
  // sanitizeSenderForSubject
  EXPECT(String(sanitizeSenderForSubject(nullptr).c_str()) == String("unknown sender"), "null->unknown");
  EXPECT(String(sanitizeSenderForSubject("").c_str()) == String("unknown sender"), "empty->unknown");
  EXPECT(String(sanitizeSenderForSubject("   ").c_str()) == String("unknown sender"), "spaces->unknown");
  EXPECT(String(sanitizeSenderForSubject("Alice").c_str()) == String("Alice"), "plain");
  // control byte -> space then trim
  EXPECT(contains(sanitizeSenderForSubject("A\x01" "B"), "A B"), "control->space");
  // non-ascii (>126) -> '?'
  {
    char s[] = {(char)0xC3, (char)0xA9, 0}; // utf8 bytes >126
    String r = sanitizeSenderForSubject(s);
    EXPECT(contains(r, "?"), "non-ascii -> ?");
  }
  // 40 cap
  {
    std::string longS(50, 'x');
    String r = sanitizeSenderForSubject(longS.c_str());
    EXPECT(r.length() == 40, "cap 40");
  }
  // trim
  EXPECT(String(sanitizeSenderForSubject("  Bob  ").c_str()) == String("Bob"), "trim");

  // buildSmsEmailFromParts — complete
  {
    String subject, body;
    buildSmsEmailFromParts("+123456789", "MyLabel", "42", "2024-01-02", "hello", true, "1", "1", subject, body);
    EXPECT(contains(subject, "SMS from +123456789"), "subject complete");
    EXPECT(!contains(subject, "INCOMPLETE"), "no incomplete tag");
    EXPECT(contains(body, "Sender: +123456789"), "body sender");
    EXPECT(contains(body, "Received on: MyLabel"), "body label");
    EXPECT(contains(body, "Modem message ID: 42"), "body id");
    EXPECT(contains(body, "hello"), "body text");
    EXPECT(!contains(body, "WARNING"), "no warning complete");
  }
  // incomplete
  {
    String subject, body;
    buildSmsEmailFromParts("Alice", "", "7", "2024-01-02", "frag", false, "1", "3", subject, body);
    EXPECT(contains(subject, "[INCOMPLETE 1/3]"), "incomplete subject tag");
    EXPECT(contains(subject, "SMS from Alice"), "incomplete subject sender");
    EXPECT(contains(body, "WARNING"), "incomplete warning");
    EXPECT(!contains(body, "Received on:"), "empty label not emitted");
  }
  // null text/id/date
  {
    String subject, body;
    buildSmsEmailFromParts(nullptr, "", nullptr, nullptr, nullptr, true, nullptr, nullptr, subject, body);
    EXPECT(contains(subject, "unknown sender"), "null sender -> unknown");
    EXPECT(contains(body, "Modem message ID: "), "null id empty");
  }
  // buildZteSmsEmail vs buildModemSmsEmail shape identical
  {
    ZteSms zs{};
    strncpy(zs.id, "10", sizeof(zs.id));
    strncpy(zs.number, "Bob", sizeof(zs.number));
    strncpy(zs.dateRaw, "rawDate", sizeof(zs.dateRaw));
    strncpy(zs.textUtf8, "zte text", sizeof(zs.textUtf8));
    zs.concatComplete = true;
    String s1,b1;
    buildZteSmsEmail(zs, "L", s1, b1);
    EXPECT(contains(b1, "zte text"), "zte wrapper");

    ModemSms ms{};
    strncpy(ms.id, "10", sizeof(ms.id));
    strncpy(ms.number, "Bob", sizeof(ms.number));
    strncpy(ms.date, "rawDate", sizeof(ms.date));
    strncpy(ms.text, "modem text", sizeof(ms.text));
    ms.concatComplete = true;
    String s2,b2;
    buildModemSmsEmail(ms, "L", s2, b2);
    EXPECT(contains(b2, "modem text"), "modem wrapper");
    // both use same alias rendering
    EXPECT(contains(b1, "Received on: L") && contains(b2, "Received on: L"), "alias shared");
  }

  printf("%d/%d email_builder tests passed\n", pass, run);
  return pass == run ? 0 : 1;
}
