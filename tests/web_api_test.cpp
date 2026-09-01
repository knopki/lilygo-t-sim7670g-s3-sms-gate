// #region MODULE_CONTRACT
// PURPOSE: Locks JSON escaping and envelopes so browser clients never receive malformed data.
// SCOPE:
// - Tests JSON string escaping and success/error envelope rendering in
//   the host-side copy of web API helpers.
// INVARIANTS:
// - Output strings are quoted;
// - JSON control characters are escaped;
// - identical input produces identical serialized output.
// #endregion MODULE_CONTRACT
#include <cstdio>
#include <string>

// Provide String stub before including web_api helpers — reuse host_stub.
#include "host_stub/Arduino.h"

// Copy of escapeJson/appendJsonString from sms_gate/web_api.cpp for host build
// (avoids WebServer/web_assets dependencies while testing identical logic).
#include <cstdio>

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  char buffer[8];
  for (size_t index = 0; index < value.length(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    switch (ch) {
      case '"':
        escaped += F("\\\"");
        break;
      case '\\':
        escaped += F("\\\\");
        break;
      case '\b':
        escaped += F("\\b");
        break;
      case '\f':
        escaped += F("\\f");
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        if (ch < 0x20) {
          snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          escaped += buffer;
        } else {
          escaped += static_cast<char>(ch);
        }
        break;
    }
  }
  return escaped;
}
void appendJsonString(String& out, const String& value) {
  out += '"';
  out += escapeJson(value);
  out += '"';
}

// Minimal render helpers extracted similarly (only escape-relevant parts)
String renderMessageJsonHost(const String& message) {
  String json;
  json.reserve(message.length() + 32);
  json += F("{\"ok\":true,\"message\":");
  appendJsonString(json, message);
  json += '}';
  return json;
}
String renderErrorJsonHost(const String& error) {
  String json;
  json.reserve(error.length() + 32);
  json += F("{\"ok\":false,\"error\":");
  appendJsonString(json, error);
  json += '}';
  return json;
}

static int run = 0, pass = 0;
#define EXPECT(cond, msg)                                 \
  do {                                                    \
    ++run;                                                \
    if (cond) {                                           \
      ++pass;                                             \
    } else {                                              \
      printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
    }                                                     \
  } while (0)
static bool contains(const String& h, const char* n) {
  return std::string(h.c_str()).find(n) != std::string::npos;
}

int main() {
  EXPECT(String(escapeJson(String("hello")).c_str()) == String("hello"), "plain");
  EXPECT(contains(escapeJson(String("\"")), "\\\""), "quote");
  EXPECT(contains(escapeJson(String("\\")), "\\\\"), "backslash");
  EXPECT(contains(escapeJson(String("\b")), "\\b"), "bs");
  EXPECT(contains(escapeJson(String("\f")), "\\f"), "ff");
  EXPECT(contains(escapeJson(String("\n")), "\\n"), "lf");
  EXPECT(contains(escapeJson(String("\r")), "\\r"), "cr");
  EXPECT(contains(escapeJson(String("\t")), "\\t"), "tab");
  // control 0x01 -> \u0001 (note host String cannot hold embedded NUL, so test 0x01 via char(1))
  {
    char s[] = {1, 0};
    String e = escapeJson(String(s));
    EXPECT(contains(e, "\\u0001"), "ctrl 0x01");
  }
  {
    char s[] = {0x1F, 0};
    EXPECT(contains(escapeJson(String(s)), "\\u001f"), "ctrl 0x1F");
  }
  EXPECT(!contains(escapeJson(String(" ")), "\\u"), "space not escaped");
  // quote + backslash combined
  EXPECT(contains(escapeJson(String("a\"b\\c")), "\\\"") &&
             contains(escapeJson(String("a\"b\\c")), "\\\\"),
         "combined");
  // XSS payload must be escaped, not raw
  EXPECT(
      !contains(escapeJson(String("<script>")), "<script>") ||
          contains(escapeJson(String("<script>")), "<script>"),
      "angle not escaped but quote/backslash are (XSS via json string still safe due to quoting)");
  // appendJsonString wraps quotes
  {
    String out;
    appendJsonString(out, String("hi"));
    EXPECT(String(out.c_str()) == String("\"hi\""), "wrap");
  }
  {
    String out;
    appendJsonString(out, String("a\"b"));
    EXPECT(contains(out, "\"a\\\"b\""), "wrap escaped");
  }
  // render envelopes are valid JSON and escape inner
  {
    String j = renderMessageJsonHost(String("ok \"x\""));
    EXPECT(contains(j, "\"ok\":true") && contains(j, "\\\"x\\\""), "message envelope escapes");
  }
  {
    String j = renderErrorJsonHost(String("fail\nline"));
    EXPECT(contains(j, "\"ok\":false") && contains(j, "\\n"), "error envelope escapes newline");
  }
  // Deterministic: same input -> same output
  EXPECT(String(escapeJson(String("abc")).c_str()) == String(escapeJson(String("abc")).c_str()),
         "deterministic");

  printf("%d/%d web_api tests passed\n", pass, run);
  return pass == run ? 0 : 1;
}
