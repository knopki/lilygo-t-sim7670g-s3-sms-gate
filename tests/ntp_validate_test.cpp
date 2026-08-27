// #region MODULE_CONTRACT
// PURPOSE: Host test for the shared NTP form sanitizer — trim, 64-character
// printable-ASCII rule, error precedence and the enabled-but-empty defaults
// shared by POST /api/setup and POST /api/ntp (ADR-0005).
// #endregion MODULE_CONTRACT
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../sms_gate/system/ntp_validate.h"

static int tests_run = 0;
static int tests_pass = 0;

#define EXPECT(cond, msg) \
  do { \
    ++tests_run; \
    if (cond) { \
      ++tests_pass; \
    } else { \
      printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
    } \
  } while (0)

// #region FUNC_runSanitize
// PURPOSE: Runs the sanitizer with test-local defaults so pass-through of the
// default arguments is observable.
static NtpSanitizeResult runSanitize(bool enabled, const char* raw1, const char* raw2, char* out1,
                                     char* out2) {
  return sanitizeNtpServers(enabled, "d1.example", "d2.example", raw1, raw2, out1, out2);
}
// #endregion FUNC_runSanitize

int main() {
  char out1[kMaxNtpServerLength + 1];
  char out2[kMaxNtpServerLength + 1];

  // Plain values are preserved.
  EXPECT(runSanitize(true, "pool.ntp.org", "time.nist.gov", out1, out2) ==
             NtpSanitizeResult::kOk,
         "plain values ok");
  EXPECT(strcmp(out1, "pool.ntp.org") == 0, "server1 preserved");
  EXPECT(strcmp(out2, "time.nist.gov") == 0, "server2 preserved");

  // Leading/trailing whitespace is trimmed before validation.
  EXPECT(runSanitize(true, "  pool.ntp.org\t\n ", "\r", out1, out2) == NtpSanitizeResult::kOk,
         "trim ok");
  EXPECT(strcmp(out1, "pool.ntp.org") == 0, "server1 trimmed");
  EXPECT(strcmp(out2, "") == 0, "whitespace-only becomes empty");

  // Enabled + both empty applies the configured defaults.
  EXPECT(runSanitize(true, "", "   ", out1, out2) == NtpSanitizeResult::kOk, "empty ok");
  EXPECT(strcmp(out1, "d1.example") == 0, "default1 applied");
  EXPECT(strcmp(out2, "d2.example") == 0, "default2 applied");

  // Enabled + one server set keeps it and leaves the other slot empty.
  EXPECT(runSanitize(true, "ntp.local", "", out1, out2) == NtpSanitizeResult::kOk,
         "one server ok");
  EXPECT(strcmp(out1, "ntp.local") == 0, "set server kept");
  EXPECT(strcmp(out2, "") == 0, "empty slot stays empty");

  // Disabled + both empty stays empty.
  EXPECT(runSanitize(false, "", "", out1, out2) == NtpSanitizeResult::kOk, "disabled empty ok");
  EXPECT(strcmp(out1, "") == 0 && strcmp(out2, "") == 0, "no defaults when disabled");

  // 64 characters fit, 65 are rejected; trailing spaces trim first.
  char long64[kMaxNtpServerLength + 1];
  std::memset(long64, 'a', kMaxNtpServerLength);
  long64[kMaxNtpServerLength] = '\0';
  EXPECT(runSanitize(true, long64, "", out1, out2) == NtpSanitizeResult::kOk, "64 chars ok");
  EXPECT(strcmp(out1, long64) == 0, "64 chars preserved");

  char long65[kMaxNtpServerLength + 2];
  std::memset(long65, 'a', kMaxNtpServerLength + 1);
  long65[kMaxNtpServerLength + 1] = '\0';
  EXPECT(runSanitize(true, long65, "", out1, out2) == NtpSanitizeResult::kTooLong,
         "65 chars rejected");
  EXPECT(runSanitize(true, "", long65, out1, out2) == NtpSanitizeResult::kTooLong,
         "server2 over-length rejected");

  char long70[kMaxNtpServerLength + 8];
  std::memset(long70, 'a', kMaxNtpServerLength);
  std::memset(long70 + kMaxNtpServerLength, ' ', 6);
  long70[kMaxNtpServerLength + 6] = '\0';
  EXPECT(runSanitize(true, long70, "", out1, out2) == NtpSanitizeResult::kOk,
         "trailing spaces trim before length rule");

  // Non-printable ASCII is rejected in either server.
  EXPECT(runSanitize(true, "bad\x01host", "", out1, out2) == NtpSanitizeResult::kNotPrintable,
         "control byte rejected");
  EXPECT(runSanitize(true, "", "bad\x7fhost", out1, out2) == NtpSanitizeResult::kNotPrintable,
         "DEL rejected");
  EXPECT(runSanitize(true, "bad\x80host", "", out1, out2) == NtpSanitizeResult::kNotPrintable,
         "high byte rejected");

  // Length is checked before printability.
  EXPECT(runSanitize(true, long65, "bad\x01host", out1, out2) == NtpSanitizeResult::kTooLong,
         "length precedence");

  // Null form input is treated as empty.
  EXPECT(runSanitize(true, nullptr, nullptr, out1, out2) == NtpSanitizeResult::kOk, "null ok");
  EXPECT(strcmp(out1, "d1.example") == 0 && strcmp(out2, "d2.example") == 0,
         "null treated as empty, defaults applied");

  printf("%s: %d/%d assertions passed\n", tests_pass == tests_run ? "PASS" : "FAIL", tests_pass,
         tests_run);
  return tests_pass == tests_run ? 0 : 1;
}
