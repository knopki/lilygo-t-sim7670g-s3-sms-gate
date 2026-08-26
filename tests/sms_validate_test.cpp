// #region MODULE_CONTRACT
// PURPOSE: Host test for sms_validate shared helpers — recipient rule and
// UTF-16 unit counting. Guards the 335-unit modem limit and the optional
// leading plus that both ZTE and SIM7670G send paths rely on.
// #endregion MODULE_CONTRACT
#include <cassert>
#include <cstdio>
#include <string>
#include "../sms_gate/system/sms_validate.h"

static int tests_run = 0;
static int tests_pass = 0;

#define EXPECT(cond, msg) do { ++tests_run; if (cond) { ++tests_pass; } else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while(0)

int main() {
  // isValidSmsRecipient
  EXPECT(isValidSmsRecipient("+123"), "plus+3digits ok");
  EXPECT(isValidSmsRecipient("123"), "3 digits ok");
  EXPECT(isValidSmsRecipient("+1234567890123456789"), "20 with plus ok (plus+19 digits)");
  EXPECT(isValidSmsRecipient("12345678901234567890"), "20 digits ok");
  // 20 chars: "+1234567890123456789" = 20 chars inc plus => 19 digits => ok
  EXPECT(!isValidSmsRecipient("12"), "too short 2");
  EXPECT(!isValidSmsRecipient(""), "empty");
  EXPECT(!isValidSmsRecipient(nullptr), "null");
  EXPECT(!isValidSmsRecipient("123456789012345678901"), "21 too long");
  EXPECT(!isValidSmsRecipient("12+34"), "plus not at start");
  EXPECT(!isValidSmsRecipient("12-34"), "dash rejected");
  EXPECT(!isValidSmsRecipient("12 34"), "space rejected");
  EXPECT(!isValidSmsRecipient("abc"), "letters rejected");
  EXPECT(!isValidSmsRecipient("++123"), "double plus");

  // smsUtf16Units — ASCII
  EXPECT(smsUtf16Units("") == 0, "empty 0");
  EXPECT(smsUtf16Units("hello") == 5, "ascii 5");
  EXPECT(smsUtf16Units(nullptr) == kSmsInvalidUnits, "null invalid");
  // BMP
  EXPECT(smsUtf16Units("café") == 4, "utf8 2-byte c3a9 counts 1");
  // Surrogate pair: U+1F600 requires 2 units (4-byte utf8)
  const char emoji[] = "\xF0\x9F\x98\x80"; // 😀
  EXPECT(smsUtf16Units(emoji) == 2, "emoji 2 units");
  std::string manyA(335, 'a');
  EXPECT(smsUtf16Units(manyA.c_str()) == 335, "335 a");
  std::string tooMany(336, 'a');
  EXPECT(smsUtf16Units(tooMany.c_str()) == 336, "336 > limit caller checks");
  // malformed
  EXPECT(smsUtf16Units("\xFF") == kSmsInvalidUnits, "0xFF invalid");
  EXPECT(smsUtf16Units("\xC0\x80") == kSmsInvalidUnits, "overlong invalid");
  EXPECT(smsUtf16Units("\xE2\x82") == kSmsInvalidUnits, "truncated 3-byte");
  EXPECT(smsUtf16Units("\xED\xA0\x80") == kSmsInvalidUnits, "surrogate half invalid");

  // combined boundary: 334 a + emoji = 336 units (335+? actually 334+2=336)
  std::string mixed = std::string(334, 'a') + std::string(emoji);
  EXPECT(smsUtf16Units(mixed.c_str()) == 336, "mixed 336");

  printf("%d/%d sms_validate tests passed\n", tests_pass, tests_run);
  return tests_pass == tests_run ? 0 : 1;
}
