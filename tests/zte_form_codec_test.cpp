// Host test for ZTE form codec: isUnreservedFormByte, appendFormEscaped,
// appendLiteral — percent-escape rules for application/x-www-form-urlencoded.
#include <assert.h>
#include <string.h>

#include <stdio.h>

#include "../sms_gate/zte_form_codec.h"

namespace {

void testIsUnreserved() {
  assert(isUnreservedFormByte('A'));
  assert(isUnreservedFormByte('Z'));
  assert(isUnreservedFormByte('a'));
  assert(isUnreservedFormByte('z'));
  assert(isUnreservedFormByte('0'));
  assert(isUnreservedFormByte('9'));
  assert(isUnreservedFormByte('-'));
  assert(isUnreservedFormByte('_'));
  assert(isUnreservedFormByte('.'));
  assert(isUnreservedFormByte('~'));
  assert(!isUnreservedFormByte('+'));
  assert(!isUnreservedFormByte(';'));
  assert(!isUnreservedFormByte('/'));
  assert(!isUnreservedFormByte('='));
  assert(!isUnreservedFormByte(' '));
  assert(!isUnreservedFormByte('&'));
  puts("testIsUnreserved ok");
}

void testAppendFormEscapedBasic() {
  char out[64];
  size_t used = 0;
  assert(appendFormEscaped("abc", out, sizeof(out), used));
  assert(strcmp(out, "abc") == 0);
  assert(used == 3);

  used = 0;
  assert(appendFormEscaped("+ ;/=", out, sizeof(out), used));
  // '+' -> %2B, ' ' -> %20, ';' -> %3B, '/' -> %2F, '=' -> %3D
  assert(strcmp(out, "%2B%20%3B%2F%3D") == 0);
  puts("testAppendFormEscapedBasic ok");
}

void testAppendFormEscapedSpecial() {
  char out[128];
  size_t used = 0;
  // Mix of unreserved and escaped
  assert(appendFormEscaped("A~Z-_.0", out, sizeof(out), used));
  assert(strcmp(out, "A~Z-_.0") == 0);

  used = 0;
  assert(appendFormEscaped("&", out, sizeof(out), used));
  assert(strcmp(out, "%26") == 0);

  used = 0;
  assert(appendFormEscaped("", out, sizeof(out), used));
  assert(strcmp(out, "") == 0);
  assert(used == 0);
  puts("testAppendFormEscapedSpecial ok");
}

void testAppendFormEscapedOverflow() {
  char out[5];
  size_t used = 0;
  // "ab" fits (2 + terminator within 5)
  assert(appendFormEscaped("ab", out, sizeof(out), used));
  assert(strcmp(out, "ab") == 0);
  // "+" needs 3 bytes plus terminator; buffer 5 has 2 used, needs 3 more -> total 5 + terminator -> overflow
  used = 0;
  char small[4];
  size_t s = 0;
  // small=4 can hold "ab" (2+1) but not "a+" (1 +3 +1 =5 >4)
  assert(appendFormEscaped("a", small, sizeof(small), s));
  assert(!appendFormEscaped("+", small, sizeof(small), s));
  // Ensure buffer still terminated after failure? The function returns false when full,
  // but previous content remains terminated.
  puts("testAppendFormEscapedOverflow ok");
}

void testAppendLiteral() {
  char out[32];
  size_t used = 0;
  assert(appendLiteral("hello", out, sizeof(out), used));
  assert(strcmp(out, "hello") == 0);
  assert(used == 5);
  assert(appendLiteral(" ", out, sizeof(out), used));
  assert(strcmp(out, "hello ") == 0);
  assert(used == 6);
  // Overflow
  char tiny[6];
  size_t t = 0;
  assert(appendLiteral("12345", tiny, sizeof(tiny), t));
  assert(t == 5);
  assert(!appendLiteral("X", tiny, sizeof(tiny), t));
  puts("testAppendLiteral ok");
}

void testCombinedSendFormFragment() {
  // Reproduce a fragment of SEND_SMS form: Number=%2B... &MessageBody=...
  char out[64];
  size_t used = 0;
  assert(appendLiteral("Number=", out, sizeof(out), used));
  assert(appendFormEscaped("+79990000000", out, sizeof(out), used));
  assert(strcmp(out, "Number=%2B79990000000") == 0);
  assert(appendLiteral("&ID=-1", out, sizeof(out), used));
  assert(strcmp(out, "Number=%2B79990000000&ID=-1") == 0);
  puts("testCombinedSendFormFragment ok");
}

}  // namespace

int main() {
  testIsUnreserved();
  testAppendFormEscapedBasic();
  testAppendFormEscapedSpecial();
  testAppendFormEscapedOverflow();
  testAppendLiteral();
  testCombinedSendFormFragment();
  puts("all zte_form_codec tests passed");
  return 0;
}
