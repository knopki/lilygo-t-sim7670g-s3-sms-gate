// #region MODULE_CONTRACT
// PURPOSE: Implements the host-testable ZTE MF79RU goform dialog over any
// ZteChannel, reproducing the hardware-proven request sequence of the
// reference forwarder (see ADR-0003 and RESEARCH notes in the forwarder
// project): one HTTP/1.1 request per command, mandatory Referer, stok
// cookie session, AD = md5(md5(cr_version+wa_inner_version) + RD), a
// lenient JSON scanner because the B02 firmware emits raw control
// characters inside string values, and the SEND_SMS shape documented from
// the modem's own web UI (ZxicSmsFwd's proven client matches it).
// INVARIANTS: Every exit path stops the channel; credentials never appear
// in stage names or error paths; every failure is traceable to one stage;
// paging stays bounded by kZteMaxPages; send bodies stay within the modem
// web UI's own UNICODE limit.
// #endregion MODULE_CONTRACT

#include "zte_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec.h"

namespace {

// #region STRUCT_JsonView
// PURPOSE: Bounds one JSON value inside the response scratch buffer so the
// scanner never needs copies or termination of the scanned region.
struct JsonView {
  const char* start;
  const char* end;
};
// #endregion STRUCT_JsonView

// #region FUNC_skipWhitespace
// PURPOSE: Advances past JSON insignificant whitespace.
const char* skipWhitespace(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
    ++p;
  }
  return p;
}
// #endregion FUNC_skipWhitespace

// #region FUNC_skipString
// PURPOSE: Advances past one quoted JSON string, accepting raw control
// characters inside it (B02 firmware behavior); returns nullptr when the
// string never terminates.
const char* skipString(const char* p, const char* end) {
  if (p >= end || *p != '"') {
    return nullptr;
  }
  ++p;
  while (p < end) {
    if (*p == '\\') {
      p += 2;
      continue;
    }
    if (*p == '"') {
      return p + 1;
    }
    ++p;
  }
  return nullptr;
}
// #endregion FUNC_skipString

// #region FUNC_isLiteralCharacter
// PURPOSE: Recognizes the character set of JSON numbers and literals so
// skipValue can pass over values this dialog never inspects.
bool isLiteralCharacter(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         ch == '-' || ch == '+' || ch == '.';
}
// #endregion FUNC_isLiteralCharacter

// #region FUNC_skipValue
// PURPOSE: Advances over one complete JSON value (string, object, array,
// number, or literal) and returns the position after it, or nullptr on a
// malformed value.
const char* skipValue(const char* p, const char* end) {
  p = skipWhitespace(p, end);
  if (p >= end) {
    return nullptr;
  }
  if (*p == '"') {
    return skipString(p, end);
  }
  if (*p == '{' || *p == '[') {
    const char open = *p;
    const char close = open == '{' ? '}' : ']';
    int depth = 1;
    ++p;
    while (p < end) {
      if (*p == '"') {
        const char* after = skipString(p, end);
        if (after == nullptr) {
          return nullptr;
        }
        p = after;
        continue;
      }
      if (*p == open) {
        ++depth;
      } else if (*p == close) {
        --depth;
        if (depth == 0) {
          return p + 1;
        }
      }
      ++p;
    }
    return nullptr;
  }
  const char* start = p;
  while (p < end && isLiteralCharacter(*p)) {
    ++p;
  }
  return p > start ? p : nullptr;
}
// #endregion FUNC_skipValue

// #region FUNC_findMember
// PURPOSE: Locates one member of a flat JSON object and bounds its value,
// skipping over (not into) nested values it passes.
bool findMember(JsonView object, const char* key, JsonView& value) {
  const char* p = skipWhitespace(object.start, object.end);
  if (p >= object.end || *p != '{') {
    return false;
  }
  ++p;
  for (;;) {
    p = skipWhitespace(p, object.end);
    if (p >= object.end || *p == '}') {
      return false;
    }
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p != '"') {
      return false;
    }
    const char* keyStart = ++p;
    while (p < object.end && *p != '"') {
      if (*p == '\\' && p + 1 < object.end) {
        ++p;
      }
      ++p;
    }
    if (p >= object.end) {
      return false;
    }
    const size_t keyLength = static_cast<size_t>(p - keyStart);
    ++p;
    p = skipWhitespace(p, object.end);
    if (p >= object.end || *p != ':') {
      return false;
    }
    ++p;
    p = skipWhitespace(p, object.end);
    const char* valueStart = p;
    const char* valueEnd = skipValue(p, object.end);
    if (valueEnd == nullptr || valueEnd == valueStart) {
      return false;
    }
    if (strlen(key) == keyLength && strncmp(keyStart, key, keyLength) == 0) {
      value.start = valueStart;
      value.end = valueEnd;
      return true;
    }
    p = valueEnd;
  }
}
// #endregion FUNC_findMember

// #region FUNC_parseHex4
// PURPOSE: Reads exactly four hex digits; returns the value or a value above
// 0x10FFFF to signal malformed input.
uint32_t parseHex4(const char* p, const char* end) {
  if (p + 4 > end) {
    return 0x200000;
  }
  uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    const char ch = p[index];
    uint32_t digit;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<uint32_t>(ch - 'A' + 10);
    } else {
      return 0x200000;
    }
    value = (value << 4) | digit;
  }
  return value;
}
// #endregion FUNC_parseHex4

// #region FUNC_appendUtf8
// PURPOSE: Appends one codepoint as UTF-8, replacing invalid codepoints and
// unpaired surrogates with U+FFFD and truncating when out is full.
void appendUtf8(uint32_t codepoint, char* out, size_t outSize, size_t& used) {
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    codepoint = 0xFFFD;
  }
  unsigned char bytes[4];
  size_t count;
  if (codepoint < 0x80) {
    bytes[0] = static_cast<unsigned char>(codepoint);
    count = 1;
  } else if (codepoint < 0x800) {
    bytes[0] = static_cast<unsigned char>(0xC0 | (codepoint >> 6));
    bytes[1] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 2;
  } else if (codepoint < 0x10000) {
    bytes[0] = static_cast<unsigned char>(0xE0 | (codepoint >> 12));
    bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[2] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 3;
  } else {
    bytes[0] = static_cast<unsigned char>(0xF0 | (codepoint >> 18));
    bytes[1] = static_cast<unsigned char>(0x80 | ((codepoint >> 12) & 0x3F));
    bytes[2] = static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F));
    bytes[3] = static_cast<unsigned char>(0x80 | (codepoint & 0x3F));
    count = 4;
  }
  for (size_t index = 0; index < count && used + 1 < outSize; ++index) {
    out[used++] = static_cast<char>(bytes[index]);
  }
}
// #endregion FUNC_appendUtf8

// #region FUNC_jsonMemberString
// PURPOSE: Reads one member as a decoded JSON string (standard escapes plus
// raw control characters) into a bounded buffer, truncating long values;
// returns false when the member is absent or not a string.
bool jsonMemberString(JsonView object, const char* key, char* out, size_t outSize) {
  JsonView value;
  if (!findMember(object, key, value)) {
    return false;
  }
  if (value.start >= value.end || *value.start != '"') {
    return false;
  }
  const char* p = value.start + 1;
  size_t used = 0;
  while (p < value.end) {
    char ch = *p++;
    if (ch == '"') {
      out[used] = '\0';
      return true;
    }
    if (ch == '\\') {
      if (p >= value.end) {
        return false;
      }
      const char escape = *p++;
      if (escape == 'u') {
        uint32_t codepoint = parseHex4(p, value.end);
        if (codepoint <= 0x10FFFF) {
          p += 4;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF && p + 6 <= value.end && p[0] == '\\' &&
              p[1] == 'u') {
            const uint32_t low = parseHex4(p + 2, value.end);
            if (low >= 0xDC00 && low <= 0xDFFF) {
              codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
              p += 6;
            } else {
              codepoint = 0xFFFD;
            }
          }
        } else {
          codepoint = 0xFFFD;
        }
        appendUtf8(codepoint, out, outSize, used);
        continue;
      }
      switch (escape) {
        case 'b':
          ch = '\b';
          break;
        case 'f':
          ch = '\f';
          break;
        case 'n':
          ch = '\n';
          break;
        case 'r':
          ch = '\r';
          break;
        case 't':
          ch = '\t';
          break;
        default:
          ch = escape;
          break;
      }
    }
    if (used + 1 < outSize) {
      out[used++] = ch;
    }
  }
  return false;  // Unterminated string: reject rather than return a prefix.
}
// #endregion FUNC_jsonMemberString

// #region CLASS_JsonArrayIterator
// PURPOSE: Walks one JSON array element by element so callers see bounded
// views without copying.
class JsonArrayIterator {
 public:
  explicit JsonArrayIterator(JsonView array) : cursor_(array.start), end_(array.end) {}

  bool next(JsonView& element) {
    cursor_ = skipWhitespace(cursor_, end_);
    if (cursor_ >= end_) {
      return false;
    }
    if (*cursor_ == '[') {
      ++cursor_;
    } else if (*cursor_ == ',') {
      ++cursor_;
      cursor_ = skipWhitespace(cursor_, end_);
    } else {
      return false;  // Not positioned on an element separator or start.
    }
    cursor_ = skipWhitespace(cursor_, end_);
    if (cursor_ < end_ && *cursor_ == ']') {
      return false;
    }
    const char* valueEnd = skipValue(cursor_, end_);
    if (valueEnd == nullptr) {
      return false;
    }
    element.start = cursor_;
    element.end = valueEnd;
    cursor_ = valueEnd;
    return true;
  }

 private:
  const char* cursor_;
  const char* end_;
};
// #endregion CLASS_JsonArrayIterator

// #region FUNC_jsonMemberArray
// PURPOSE: Bounds the value of one member when it is an array (the
// sms_data_total messages list).
bool jsonMemberArray(JsonView object, const char* key, JsonView& array) {
  return findMember(object, key, array) && array.start < array.end && *array.start == '[';
}
// #endregion FUNC_jsonMemberArray

// #region FUNC_isHexDigit
// PURPOSE: Recognizes one hexadecimal digit of the UCS-2 content encoding.
bool isHexDigit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
// #endregion FUNC_isHexDigit

// #region FUNC_isUnreservedFormByte
// PURPOSE: Recognizes the characters application/x-www-form-urlencoded may
// carry literally; every other byte (including '+' and ';') is percent-
// escaped so the modem receives the exact values.
bool isUnreservedFormByte(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
         ch == '-' || ch == '_' || ch == '.' || ch == '~';
}
// #endregion FUNC_isUnreservedFormByte

// #region FUNC_appendFormEscaped
// PURPOSE: Appends one form-value percent-escaping every non-unreserved
// byte, always terminating the buffer (the SEND_SMS form ends with an
// escaped value, so an unterminated tail would leak stack bytes onto the
// wire and into the Content-Length); returns false when the destination is
// full.
bool appendFormEscaped(const char* value, char* out, size_t outSize, size_t& used) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const char* p = value; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (isUnreservedFormByte(ch)) {
      if (used + 1 >= outSize) {
        return false;
      }
      out[used++] = static_cast<char>(ch);
    } else {
      if (used + 3 >= outSize) {
        return false;
      }
      out[used++] = '%';
      out[used++] = kHex[ch >> 4];
      out[used++] = kHex[ch & 0x0F];
    }
  }
  out[used] = '\0';  // Both append branches leave used <= outSize - 1.
  return true;
}
// #endregion FUNC_appendFormEscaped

// #region FUNC_appendLiteral
// PURPOSE: Appends one fixed fragment after an escaped value; returns false
// when the destination is full.
bool appendLiteral(const char* literal, char* out, size_t outSize, size_t& used) {
  const size_t length = strlen(literal);
  if (used + length + 1 > outSize) {
    return false;
  }
  memcpy(out + used, literal, length);
  used += length;
  out[used] = '\0';
  return true;
}
// #endregion FUNC_appendLiteral

// #region FUNC_isUcs2HexView
// PURPOSE: Validates the whole content view as 4-hex-per-codepoint UCS-2
// before any decoding, so partial garbage never becomes text.
bool isUcs2HexView(JsonView view) {
  if (static_cast<size_t>(view.end - view.start) % 4 != 0) {
    return false;
  }
  for (const char* p = view.start; p < view.end; ++p) {
    if (!isHexDigit(*p)) {
      return false;
    }
  }
  return true;
}
// #endregion FUNC_isUcs2HexView

// #region FUNC_decodeUcs2HexView
// PURPOSE: Decodes validated UCS-2 hex into UTF-8 (surrogate pairs joined,
// unpaired surrogates replaced with U+FFFD, output truncated at outSize).
size_t decodeUcs2HexView(JsonView view, char* out, size_t outSize) {
  size_t used = 0;
  const char* p = view.start;
  while (p + 4 <= view.end) {
    uint32_t codepoint = parseHex4(p, view.end);
    p += 4;
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && p + 4 <= view.end) {
      const uint32_t low = parseHex4(p, view.end);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
        p += 4;
      }
    }
    appendUtf8(codepoint, out, outSize, used);
  }
  out[used] = '\0';
  return used;
}
// #endregion FUNC_decodeUcs2HexView

// #region FUNC_copyRawView
// PURPOSE: Copies a non-hex content value verbatim (the reference forwarder
// falls back to the raw string), truncating at outSize.
void copyRawView(JsonView view, char* out, size_t outSize) {
  size_t used = 0;
  for (const char* p = view.start; p < view.end && used + 1 < outSize; ++p) {
    out[used++] = *p;
  }
  out[used] = '\0';
}
// #endregion FUNC_copyRawView

// #region FUNC_innerStringValue
// PURPOSE: Strips the quotes of a string JSON value to bound its raw inner
// bytes for content decoding.
bool innerStringValue(JsonView value, JsonView& inner) {
  if (value.start >= value.end || *value.start != '"' || *(value.end - 1) != '"' ||
      value.end - value.start < 2) {
    return false;
  }
  inner.start = value.start + 1;
  inner.end = value.end - 1;
  return true;
}
// #endregion FUNC_innerStringValue

// #region FUNC_parseUint32String
// PURPOSE: Converts a digits-only decimal string (message IDs, capacity
// counters) to uint32 for ordering and reporting decisions.
bool parseUint32String(const char* id, uint32_t& out) {
  if (*id == '\0') {
    return false;
  }
  uint32_t value = 0;
  for (const char* p = id; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    if (value > 429496729UL) {
      return false;
    }
    value = value * 10 + static_cast<uint32_t>(*p - '0');
  }
  out = value;
  return true;
}
// #endregion FUNC_parseUint32String

// #region FUNC_captureMessage
// PURPOSE: Extracts one complete incoming SMS (including the decoded text)
// from its JSON object view.
bool captureMessage(JsonView element, ZteSms& out) {
  if (!jsonMemberString(element, "id", out.id, sizeof(out.id))) {
    return false;
  }
  if (!jsonMemberString(element, "number", out.number, sizeof(out.number))) {
    out.number[0] = '\0';
  }
  if (!jsonMemberString(element, "date", out.dateRaw, sizeof(out.dateRaw))) {
    out.dateRaw[0] = '\0';
  }
  char complete[8];
  out.concatComplete = true;
  if (jsonMemberString(element, "received_all_concat_sms", complete, sizeof(complete))) {
    out.concatComplete = strcmp(complete, "1") == 0;
  }
  if (!jsonMemberString(element, "concat_sms_received", out.concatReceived,
                        sizeof(out.concatReceived))) {
    out.concatReceived[0] = '\0';
  }
  if (!jsonMemberString(element, "concat_sms_total", out.concatTotal, sizeof(out.concatTotal))) {
    out.concatTotal[0] = '\0';
  }
  JsonView content;
  JsonView inner;
  if (findMember(element, "content", content) && innerStringValue(content, inner)) {
    if (isUcs2HexView(inner)) {
      decodeUcs2HexView(inner, out.textUtf8, sizeof(out.textUtf8));
    } else {
      copyRawView(inner, out.textUtf8, sizeof(out.textUtf8));
    }
  } else {
    out.textUtf8[0] = '\0';
  }
  return true;
}
// #endregion FUNC_captureMessage

// #region FUNC_parseEntryHeader
// PURPOSE: Reads the id and tag one scan step needs (tag optional: a
// missing tag simply marks the entry not incoming), so order decisions and
// the incoming filter never decode whole messages.
bool parseEntryHeader(JsonView element, char* id, size_t idSize, char* tag, size_t tagSize) {
  if (!jsonMemberString(element, "id", id, idSize)) {
    return false;
  }
  if (!jsonMemberString(element, "tag", tag, tagSize)) {
    tag[0] = '\0';
  }
  return true;
}
// #endregion FUNC_parseEntryHeader

// #region FUNC_headerHasPrefix
// PURPOSE: Case-insensitive header-name match for the three HTTP headers
// this dialog inspects.
bool headerHasPrefix(const char* line, const char* prefix) {
  size_t index = 0;
  for (; prefix[index] != '\0'; ++index) {
    char left = line[index];
    char right = prefix[index];
    if (left >= 'a' && left <= 'z') {
      left = static_cast<char>(left - 'a' + 'A');
    }
    if (right >= 'a' && right <= 'z') {
      right = static_cast<char>(right - 'a' + 'A');
    }
    if (left != right) {
      return false;
    }
  }
  return line[index] == '\0' || line[index] == ' ' || line[index] == ':';
}
// #endregion FUNC_headerHasPrefix

// #region FUNC_parseContentLength
// PURPOSE: Parses the Content-Length value; returns false on non-digits.
bool parseContentLength(const char* value, size_t& out) {
  size_t number = 0;
  if (*value == '\0') {
    return false;
  }
  for (const char* p = value; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    number = number * 10 + static_cast<size_t>(*p - '0');
  }
  out = number;
  return true;
}
// #endregion FUNC_parseContentLength

}  // namespace

// #region FUNC_formatZteDate
// PURPOSE: Gives email rendering and future callers one readable timestamp
// instead of raw modem fields; an unexpected firmware shape passes through
// verbatim rather than being dropped.
bool formatZteDate(const char* raw, char* out, size_t outSize) {
  if (raw == nullptr || out == nullptr || outSize == 0) {
    return false;
  }
  int yy = 0;
  int mm = 0;
  int dd = 0;
  int hh = 0;
  int mi = 0;
  int ss = 0;
  int tzQuarters = 0;
  int consumed = -1;
  const int matched =
      sscanf(raw, "%d,%d,%d,%d,%d,%d,%d%n", &yy, &mm, &dd, &hh, &mi, &ss, &tzQuarters, &consumed);
  const bool shape = matched == 7 && consumed >= 0 &&
                     static_cast<size_t>(consumed) == strlen(raw) && yy >= 0 && yy <= 99 &&
                     mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31 && hh >= 0 && hh <= 23 && mi >= 0 &&
                     mi <= 59 && ss >= 0 && ss <= 59 && tzQuarters >= -96 && tzQuarters <= 96;
  if (!shape) {
    snprintf(out, outSize, "%s", raw);
    return false;
  }
  const unsigned tzAbs = static_cast<unsigned>(tzQuarters < 0 ? -tzQuarters : tzQuarters);
  snprintf(out, outSize, "20%02d-%02d-%02d %02d:%02d:%02d UTC%+03d:%02u", yy, mm, dd, hh, mi, ss,
           tzQuarters < 0 ? -static_cast<int>(tzAbs / 4) : static_cast<int>(tzAbs / 4),
           (tzAbs % 4) * 15U);
  return true;
}
// #endregion FUNC_formatZteDate

// #region FUNC_zteSmsUtf16Units
// PURPOSE: Shares one length rule (and one UTF-8 validity gate) between the
// web form validation and sendSms so the encoded body always fits the modem
// web UI's own send limit. Delegates to the shared sms_validate.h helper so
// ZTE and SIM7670G share the same 335-unit limit.
size_t zteSmsUtf16Units(const char* utf8) { return smsUtf16Units(utf8); }
// #endregion FUNC_zteSmsUtf16Units

// #region FUNC_ZteModem_ZteModem
// PURPOSE: Binds one dialog instance to one channel and response scratch
// buffer for its lifetime.
ZteModem::ZteModem(ZteChannel& channel, char* scratch, size_t scratchSize)
    : channel_(channel), scratch_(scratch), scratchSize_(scratchSize) {
  host_[0] = '\0';
  password_[0] = '\0';
  cookie_[0] = '\0';
  waVersion_[0] = '\0';
}
// #endregion FUNC_ZteModem_ZteModem

// #region FUNC_ZteModem_fail
// PURPOSE: Records the stable stage token of the last failure.
void ZteModem::fail(const char* stage) { failedStage_ = stage; }
// #endregion FUNC_ZteModem_fail

// #region FUNC_ZteModem_requestPost
// PURPOSE: Sends one POST to goform_set_cmd_process with the mandatory
// Referer header, the session cookie when present, and Connection: close,
// then reads the response into the scratch buffer; the buffer fits the
// SEND_SMS form (a 335-unit body encodes to 1340 hex characters).
ZteResult ZteModem::requestPost(const char* formBody) {
  char request[2560];
  size_t used = static_cast<size_t>(snprintf(request, sizeof(request),
                                             "POST /goform/goform_set_cmd_process HTTP/1.1\r\n"
                                             "Host: %s\r\n"
                                             "Referer: http://%s/index.html\r\n",
                                             host_, host_));
  if (used >= sizeof(request)) {
    fail("http_request");
    return ZteResult::kHttpFailed;
  }
  if (hasSession()) {
    used += static_cast<size_t>(
        snprintf(request + used, sizeof(request) - used, "Cookie: %s\r\n", cookie_));
    if (used >= sizeof(request)) {
      fail("http_request");
      return ZteResult::kHttpFailed;
    }
  }
  used += static_cast<size_t>(
      snprintf(request + used, sizeof(request) - used,
               "Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n%s",
               static_cast<unsigned>(strlen(formBody)), formBody));
  if (used >= sizeof(request)) {
    fail("http_request");
    return ZteResult::kHttpFailed;
  }

  channel_.stop();
  if (!channel_.connect(host_, kZteHttpPort)) {
    fail("connect");
    channel_.stop();
    return ZteResult::kConnectFailed;
  }
  const bool written = channel_.write(request, used);
  ZteResult result = ZteResult::kHttpFailed;
  if (written) {
    result = readResponse();
  } else {
    fail("http_write");
  }
  channel_.stop();
  return result;
}
// #endregion FUNC_ZteModem_requestPost

// #region FUNC_ZteModem_requestGet
// PURPOSE: Sends one GET to goform_get_cmd_process with the same mandatory
// headers and reads the response into the scratch buffer.
ZteResult ZteModem::requestGet(const char* query) {
  char request[768];
  size_t used = static_cast<size_t>(snprintf(request, sizeof(request),
                                             "GET /goform/goform_get_cmd_process?%s HTTP/1.1\r\n"
                                             "Host: %s\r\n"
                                             "Referer: http://%s/index.html\r\n",
                                             query, host_, host_));
  if (used >= sizeof(request)) {
    fail("http_request");
    return ZteResult::kHttpFailed;
  }
  if (hasSession()) {
    used += static_cast<size_t>(
        snprintf(request + used, sizeof(request) - used, "Cookie: %s\r\n", cookie_));
    if (used >= sizeof(request)) {
      fail("http_request");
      return ZteResult::kHttpFailed;
    }
  }
  used += static_cast<size_t>(
      snprintf(request + used, sizeof(request) - used, "Connection: close\r\n\r\n"));
  if (used >= sizeof(request)) {
    fail("http_request");
    return ZteResult::kHttpFailed;
  }

  channel_.stop();
  if (!channel_.connect(host_, kZteHttpPort)) {
    fail("connect");
    channel_.stop();
    return ZteResult::kConnectFailed;
  }
  const bool written = channel_.write(request, used);
  ZteResult result = ZteResult::kHttpFailed;
  if (written) {
    result = readResponse();
  } else {
    fail("http_write");
  }
  channel_.stop();
  return result;
}
// #endregion FUNC_ZteModem_requestGet

// #region FUNC_ZteModem_readResponse
// PURPOSE: Consumes one HTTP response: requires 200, captures the stok
// cookie, and fills the scratch buffer with the exact body (or until EOF
// when Content-Length is absent).
ZteResult ZteModem::readResponse() {
  bodyLength_ = 0;
  char line[256];
  int length = channel_.readLine(line, sizeof(line));
  if (length < 12 || strncmp(line, "HTTP/1.", 7) != 0 || line[7] < '0' || line[7] > '9' ||
      line[8] != ' ' || line[9] < '0' || line[9] > '9' || line[10] < '0' || line[10] > '9' ||
      line[11] < '0' || line[11] > '9') {
    fail("http_status");
    return ZteResult::kHttpFailed;
  }
  const int status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
  if (status != 200) {
    fail("http_status");
    return ZteResult::kHttpFailed;
  }

  size_t contentLength = 0;
  bool hasContentLength = false;
  bool chunked = false;
  for (;;) {
    length = channel_.readLine(line, sizeof(line));
    if (length < 0) {
      fail("http_header");
      return ZteResult::kHttpFailed;
    }
    if (length == 0) {
      break;
    }
    if (headerHasPrefix(line, "Set-Cookie:")) {
      const char* value = line + 11;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      if (strncmp(value, "stok=", 5) == 0) {
        size_t used = 0;
        for (const char* p = value; used + 1 < sizeof(cookie_); ++p) {
          if (*p == '\0' || *p == ';') {
            break;
          }
          cookie_[used++] = *p;
        }
        cookie_[used] = '\0';
      }
    } else if (headerHasPrefix(line, "Content-Length:")) {
      const char* value = line + 15;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      hasContentLength = parseContentLength(value, contentLength);
    } else if (headerHasPrefix(line, "Transfer-Encoding:")) {
      const char* value = line + 18;
      if (strstr(value, "chunked") != nullptr) {
        chunked = true;
      }
    }
  }
  if (chunked) {
    fail("http_chunked");
    return ZteResult::kProtocolError;
  }

  if (hasContentLength) {
    if (contentLength >= scratchSize_) {
      fail("http_body");
      return ZteResult::kHttpFailed;
    }
    size_t remaining = contentLength;
    char* destination = scratch_;
    while (remaining > 0) {
      const int got = channel_.read(destination, remaining);
      if (got <= 0) {
        fail("http_body");
        return ZteResult::kHttpFailed;
      }
      destination += got;
      remaining -= static_cast<size_t>(got);
    }
    *destination = '\0';
    bodyLength_ = contentLength;
    return ZteResult::kSuccess;
  }

  size_t used = 0;
  while (used + 1 < scratchSize_) {
    const int got = channel_.read(scratch_ + used, scratchSize_ - 1 - used);
    if (got <= 0) {
      break;
    }
    used += static_cast<size_t>(got);
  }
  if (used == 0) {
    fail("http_body");
    return ZteResult::kHttpFailed;
  }
  scratch_[used] = '\0';
  bodyLength_ = used;
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_readResponse

// #region FUNC_ZteModem_loginSession
// PURPOSE: Performs one LOGIN with the base64 password and immediately
// loads the firmware versions, mirroring the reference sequence.
ZteResult ZteModem::loginSession() {
  cookie_[0] = '\0';
  char encoded[(((kMaxZtePasswordLength + 2) / 3) * 4) + 1];
  if (codec::encodeBase64(password_, strlen(password_), encoded, sizeof(encoded)) == 0) {
    fail("login_encode");
    return ZteResult::kProtocolError;
  }
  char formBody[192];
  snprintf(formBody, sizeof(formBody), "isTest=false&goformId=LOGIN&password=%s", encoded);
  ZteResult result = requestPost(formBody);
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char value[16];
  if (!jsonMemberString(body, "result", value, sizeof(value)) || strcmp(value, "0") != 0) {
    fail("login");
    return ZteResult::kLoginRejected;
  }
  if (!hasSession()) {
    fail("login_cookie");
    return ZteResult::kLoginRejected;
  }
  return requestVersions();
}
// #endregion FUNC_ZteModem_loginSession

// #region FUNC_ZteModem_openSession
// PURPOSE: Opens a session with one retry when the versions answer arrives
// stale, matching the reference forwarder's single relogin.
ZteResult ZteModem::openSession() {
  ZteResult result = loginSession();
  if (result == ZteResult::kStaleSession) {
    result = loginSession();
  }
  return result;
}
// #endregion FUNC_ZteModem_openSession

// #region FUNC_ZteModem_requestVersions
// PURPOSE: Loads cr_version and wa_inner_version whose concatenation feeds
// the AD token; an absent version marks the session unusable.
ZteResult ZteModem::requestVersions() {
  ZteResult result = requestGet("isTest=false&cmd=cr_version,wa_inner_version&multi_data=1");
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char cr[32];
  char wa[64];
  if (!jsonMemberString(body, "cr_version", cr, sizeof(cr))) {
    cr[0] = '\0';
  }
  if (!jsonMemberString(body, "wa_inner_version", wa, sizeof(wa))) {
    wa[0] = '\0';
  }
  snprintf(waVersion_, sizeof(waVersion_), "%s%s", cr, wa);
  if (waVersion_[0] == '\0') {
    fail("version");
    return ZteResult::kStaleSession;
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_requestVersions

// #region FUNC_ZteModem_login
// PURPOSE: Stores the credentials and opens the first session.
ZteResult ZteModem::login(const char* host, const char* password) {
  snprintf(host_, sizeof(host_), "%s", host);
  snprintf(password_, sizeof(password_), "%s", password);
  return openSession();
}
// #endregion FUNC_ZteModem_login

// #region FUNC_ZteModem_fetchRd
// PURPOSE: Reads one fresh RD token in the current session, relogging in
// once when the answer arrives stale.
ZteResult ZteModem::fetchRd(char* rd, size_t rdSize) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    ZteResult result = requestGet("isTest=false&cmd=RD");
    if (result == ZteResult::kSuccess) {
      JsonView body{scratch_, scratch_ + bodyLength_};
      if (jsonMemberString(body, "RD", rd, rdSize) && rd[0] != '\0') {
        return ZteResult::kSuccess;
      }
    } else if (result != ZteResult::kHttpFailed) {
      return result;
    }
    result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  fail("rd");
  return ZteResult::kStaleSession;
}
// #endregion FUNC_ZteModem_fetchRd

// #region FUNC_ZteModem_fetchAd
// PURPOSE: Builds the AD anti-CSRF token shared by every set command
// (AD = md5(md5(wa_version) + RD)) from one fresh RD token.
ZteResult ZteModem::fetchAd(char* ad, size_t adSize) {
  if (adSize < 33) {
    fail("ad");
    return ZteResult::kProtocolError;
  }
  char rd[80];
  ZteResult result = fetchRd(rd, sizeof(rd));
  if (result != ZteResult::kSuccess) {
    return result;
  }
  char inner[33];
  codec::md5Hex(waVersion_, strlen(waVersion_), inner);
  char concatenated[33 + sizeof(rd) + 1];
  snprintf(concatenated, sizeof(concatenated), "%s%s", inner, rd);
  codec::md5Hex(concatenated, strlen(concatenated), ad);
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_fetchAd

// #region FUNC_ZteModem_fetchSmsPage
// PURPOSE: Requests one device-storage list into the scratch buffer with a
// caller-selected modem tag filter, relogging once when the modem answers
// the stale empty shape. B02 ignores page, so callers that delete records
// intentionally re-read page zero after each deletion.
ZteResult ZteModem::fetchSmsPage(unsigned int page, const char* tags) {
  if (tags == nullptr || tags[0] == '\0') {
    fail("sms_list");
    return ZteResult::kProtocolError;
  }
  char query[192];
  snprintf(query, sizeof(query),
           "isTest=false&cmd=sms_data_total&page=%u&data_per_page=%u&mem_store=1&tags=%s&"
           "order_by=order+by+id+asc",
           page, static_cast<unsigned>(kZtePageSize), tags);
  for (int attempt = 0; attempt < 2; ++attempt) {
    ZteResult result = requestGet(query);
    if (result == ZteResult::kSuccess) {
      JsonView body{scratch_, scratch_ + bodyLength_};
      JsonView messages;
      if (jsonMemberArray(body, "messages", messages)) {
        return ZteResult::kSuccess;
      }
    } else if (result != ZteResult::kHttpFailed) {
      return result;
    }
    result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  fail("sms_list");
  return ZteResult::kStaleSession;
}
// #endregion FUNC_ZteModem_fetchSmsPage

// #region FUNC_ZteModem_scanOldest
// PURPOSE: Walks the inbox pages and captures the oldest incoming SMS,
// auto-detecting the effective ordering from consecutive IDs so a firmware
// that ignores the requested ascending order still yields the oldest
// message.
ZteResult ZteModem::scanOldest(ZteSms& out, bool& found) {
  found = false;
  bool ascendingDetected = false;
  bool ascending = true;
  for (unsigned int page = 0; page < kZteMaxPages; ++page) {
    ZteResult result = fetchSmsPage(page, "10");
    if (result != ZteResult::kSuccess) {
      return result;
    }
    JsonView body{scratch_, scratch_ + bodyLength_};
    JsonView messages;
    if (!jsonMemberArray(body, "messages", messages)) {
      fail("sms_list");
      return ZteResult::kProtocolError;
    }
    size_t count = 0;
    uint32_t previousId = 0;
    JsonArrayIterator iterator(messages);
    JsonView element;
    while (iterator.next(element)) {
      ++count;
      char id[24];
      char tag[8];
      if (!parseEntryHeader(element, id, sizeof(id), tag, sizeof(tag))) {
        fail("sms_entry");
        return ZteResult::kProtocolError;
      }
      uint32_t idValue = 0;
      if (!parseUint32String(id, idValue)) {
        fail("sms_entry");
        return ZteResult::kProtocolError;
      }
      if (count >= 2 && !ascendingDetected) {
        ascendingDetected = true;
        ascending = previousId < idValue;
      }
      previousId = idValue;
      if (tag[0] == '0' || tag[0] == '1') {
        if (!found || (ascendingDetected && !ascending)) {
          if (!captureMessage(element, out)) {
            fail("sms_entry");
            return ZteResult::kProtocolError;
          }
          found = true;
        }
      }
    }
    if (ascendingDetected && ascending && found) {
      return ZteResult::kSuccess;  // First page hit is already the oldest.
    }
    if (count < kZtePageSize) {
      return ZteResult::kSuccess;  // Final page reached.
    }
  }
  return ZteResult::kSuccess;  // Page cap reached; retry continues next poll.
}
// #endregion FUNC_ZteModem_scanOldest

// #region FUNC_ZteModem_findOldestIncoming
// PURPOSE: Ensures a session exists, then scans for the oldest incoming SMS.
ZteResult ZteModem::findOldestIncoming(ZteSms& out, bool& found) {
  if (!hasSession()) {
    ZteResult result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  return scanOldest(out, found);
}
// #endregion FUNC_ZteModem_findOldestIncoming

// #region FUNC_ZteModem_findOutgoing
// PURPOSE: Finds one terminal outgoing record (tag 2 sent or tag 3 failed)
// across bounded device-storage pages. The caller deletes it and scans again
// from page zero, so page shifts caused by deletion cannot skip a record.
ZteResult ZteModem::findOutgoing(const char* tag, char* id, size_t idSize, bool& found) {
  found = false;
  if (tag == nullptr || (strcmp(tag, "2") != 0 && strcmp(tag, "3") != 0) || id == nullptr ||
      idSize == 0) {
    fail("outgoing_input");
    return ZteResult::kProtocolError;
  }
  // The B02 ignores page; the tag filter makes its page zero a bounded
  // work queue, and deletion shifts the next matching record into it.
  ZteResult result = fetchSmsPage(0, tag);
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  JsonView messages;
  if (!jsonMemberArray(body, "messages", messages)) {
    fail("sms_list");
    return ZteResult::kProtocolError;
  }
  JsonArrayIterator iterator(messages);
  JsonView element;
  while (iterator.next(element)) {
    char entryId[24];
    char entryTag[8];
    if (!parseEntryHeader(element, entryId, sizeof(entryId), entryTag, sizeof(entryTag)) ||
        strlen(entryId) + 1 > idSize) {
      fail("sms_entry");
      return ZteResult::kProtocolError;
    }
    if (strcmp(entryTag, tag) == 0) {
      snprintf(id, idSize, "%s", entryId);
      found = true;
      return ZteResult::kSuccess;
    }
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_findOutgoing

// #region FUNC_ZteModem_verifyAbsent
// PURPOSE: Confirms a deleted ID disappeared from the same filter that
// selected it. B02 ignores page, but a selected record must remain in this
// filter's first page if DELETE_SMS did not remove it.
ZteResult ZteModem::verifyAbsent(const char* targetId, const char* tags) {
  uint32_t target = 0;
  if (targetId == nullptr || !parseUint32String(targetId, target) || tags == nullptr ||
      tags[0] == '\0') {
    fail("delete_verify");
    return ZteResult::kProtocolError;
  }
  ZteResult result = fetchSmsPage(0, tags);
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  JsonView messages;
  if (!jsonMemberArray(body, "messages", messages)) {
    fail("sms_list");
    return ZteResult::kProtocolError;
  }
  JsonArrayIterator iterator(messages);
  JsonView element;
  while (iterator.next(element)) {
    char id[24];
    char tag[8];
    if (!parseEntryHeader(element, id, sizeof(id), tag, sizeof(tag))) {
      fail("sms_entry");
      return ZteResult::kProtocolError;
    }
    uint32_t idValue = 0;
    if (!parseUint32String(id, idValue)) {
      fail("sms_entry");
      return ZteResult::kProtocolError;
    }
    if (idValue == target) {
      fail("delete_unverified");
      return ZteResult::kProtocolError;
    }
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_verifyAbsent

// #region FUNC_ZteModem_deleteMessage
// PURPOSE: Deletes one validated message ID with a fresh AD token and
// verifies it disappeared so a silently failed delete cannot be assumed
// complete by either the incoming forwarder or outgoing cleanup.
ZteResult ZteModem::deleteMessage(const char* id, const char* verifyTags) {
  uint32_t numericId = 0;
  if (id == nullptr || !parseUint32String(id, numericId) || verifyTags == nullptr ||
      verifyTags[0] == '\0') {
    fail("delete_input");
    return ZteResult::kProtocolError;
  }
  if (!hasSession()) {
    ZteResult result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  char ad[33];
  ZteResult result = fetchAd(ad, sizeof(ad));
  if (result != ZteResult::kSuccess) {
    return result;
  }
  char formBody[192];
  snprintf(formBody, sizeof(formBody),
           "isTest=false&goformId=DELETE_SMS&msg_id=%s%%3B&notCallback=true&AD=%s", id, ad);
  result = requestPost(formBody);
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char value[16];
  if (!jsonMemberString(body, "result", value, sizeof(value)) || strcmp(value, "success") != 0) {
    fail("delete");
    return ZteResult::kProtocolError;
  }
  return verifyAbsent(id, verifyTags);
}
// #endregion FUNC_ZteModem_deleteMessage

// #region FUNC_ZteModem_deleteSms
// PURPOSE: Keeps the incoming-forwarding contract focused on one captured
// SMS while sharing the proven delete-and-verify operation with cleanup.
ZteResult ZteModem::deleteSms(const ZteSms& sms) { return deleteMessage(sms.id, "10"); }
// #endregion FUNC_ZteModem_deleteSms

// #region FUNC_ZteModem_cleanupOutgoing
// PURPOSE: Reclaims all final outgoing records after a terminal send result.
// B02 ignores page, so each tag-specific page-zero response is a bounded
// work queue: after a verified deletion the next matching ID shifts into it.
ZteResult ZteModem::cleanupOutgoing(uint16_t& deleted) {
  deleted = 0;
  if (!hasSession()) {
    ZteResult result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  static constexpr const char* kOutgoingTags[] = {"2", "3"};
  for (const char* tag : kOutgoingTags) {
    for (;;) {
      if (deleted >= kZteMaxPages * kZtePageSize) {
        fail("outgoing_cleanup_limit");
        return ZteResult::kProtocolError;
      }
      char id[kZteSmsIdLength + 1];
      bool found = false;
      ZteResult result = findOutgoing(tag, id, sizeof(id), found);
      if (result != ZteResult::kSuccess || !found) {
        if (result != ZteResult::kSuccess) {
          return result;
        }
        break;
      }
      result = deleteMessage(id, tag);
      if (result != ZteResult::kSuccess) {
        return result;
      }
      ++deleted;
    }
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_cleanupOutgoing

// #region FUNC_ZteModem_readInboxStatus
// PURPOSE: Reads the device-storage occupancy for the operator test route.
ZteResult ZteModem::readInboxStatus(ZteInboxStatus& out) {
  ZteResult result = requestGet("isTest=false&cmd=sms_capacity_info");
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char used[8];
  char total[8];
  if (!jsonMemberString(body, "sms_nvused_total", used, sizeof(used)) ||
      !jsonMemberString(body, "sms_nv_total", total, sizeof(total))) {
    fail("capacity");
    return ZteResult::kProtocolError;
  }
  uint32_t usedValue = 0;
  uint32_t totalValue = 0;
  if (!parseUint32String(used, usedValue) || !parseUint32String(total, totalValue)) {
    fail("capacity");
    return ZteResult::kProtocolError;
  }
  out.used = static_cast<uint16_t>(usedValue);
  out.total = static_cast<uint16_t>(totalValue);
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_readInboxStatus

// #region FUNC_buildSmsTimeString
// PURPOSE: Renders the sms_time field in the shape the modem's own web UI
// sends (yy;mm;dd;HH;MM;SS;+tz with an unpadded hour offset like the
// browser's "+3"). Without a synced clock the epoch placeholder goes out
// instead of a wrong wall-clock guess.
void buildSmsTimeString(char* out, size_t outSize) {
  const time_t now = time(nullptr);
  if (now < 1577836800) {  // Before 2020-01-01: no synced clock available.
    snprintf(out, outSize, "00;01;01;00;00;00;+0");
    return;
  }
  struct tm parts;
  gmtime_r(&now, &parts);
  snprintf(out, outSize, "%02d;%02d;%02d;%02d;%02d;%02d;+0", (parts.tm_year + 1900) % 100,
           parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
}
// #endregion FUNC_buildSmsTimeString

// #region FUNC_ZteModem_sendSms
// PURPOSE: Sends one SMS with the request shape documented from the modem's
// own web UI and proven by the reference clients: a fresh AD token, the
// percent-escaped Number and sms_time, the UCS-2-hex MessageBody, ID=-1,
// and encode_type=UNICODE; success means accepted, not yet delivered.
ZteResult ZteModem::sendSms(const char* number, const char* textUtf8) {
  if (number == nullptr || textUtf8 == nullptr) {
    fail("send_input");
    return ZteResult::kProtocolError;
  }
  const size_t numberLength = strlen(number);
  if (numberLength == 0 || numberLength > kZteNumberLength) {
    fail("send_input");
    return ZteResult::kProtocolError;
  }
  for (size_t index = 0; index < numberLength; ++index) {
    const unsigned char ch = static_cast<unsigned char>(number[index]);
    if (ch < 32 || ch > 126) {
      fail("send_input");
      return ZteResult::kProtocolError;
    }
  }
  const size_t units = zteSmsUtf16Units(textUtf8);
  if (units == 0 || units == kZteSmsInvalidUnits || units > kMaxZteSmsSendUnits) {
    fail("send_input");
    return ZteResult::kProtocolError;
  }
  if (!hasSession()) {
    ZteResult result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
  }
  char ad[33];
  ZteResult result = fetchAd(ad, sizeof(ad));
  if (result != ZteResult::kSuccess) {
    return result;
  }

  char hexBody[(kMaxZteSmsSendUnits * 4) + 1];
  codec::encodeUcs2Hex(textUtf8, hexBody, sizeof(hexBody));
  // Mirror the modem's own web UI: printable-ASCII text transmits as
  // GSM7_default, anything else as UNICODE (the browser's proven request
  // carries the same UTF-16-hex body under both labels).
  bool asciiOnly = true;
  for (const char* p = textUtf8; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 0x20 || ch > 0x7E) {
      asciiOnly = false;
      break;
    }
  }
  const char* encodeType = asciiOnly ? "GSM7_default" : "UNICODE";
  char smsTime[32];
  buildSmsTimeString(smsTime, sizeof(smsTime));
  char formBody[1792];
  size_t used = 0;
  if (!appendLiteral("isTest=false&goformId=SEND_SMS&notCallback=true&Number=", formBody,
                     sizeof(formBody), used) ||
      !appendFormEscaped(number, formBody, sizeof(formBody), used) ||
      !appendLiteral("&sms_time=", formBody, sizeof(formBody), used) ||
      !appendFormEscaped(smsTime, formBody, sizeof(formBody), used) ||
      !appendLiteral("&MessageBody=", formBody, sizeof(formBody), used) ||
      !appendFormEscaped(hexBody, formBody, sizeof(formBody), used) ||
      !appendLiteral("&ID=-1&encode_type=", formBody, sizeof(formBody), used) ||
      !appendFormEscaped(encodeType, formBody, sizeof(formBody), used) ||
      !appendLiteral("&AD=", formBody, sizeof(formBody), used) ||
      !appendFormEscaped(ad, formBody, sizeof(formBody), used)) {
    fail("send_form");
    return ZteResult::kProtocolError;
  }
  snprintf(lastSendForm_, sizeof(lastSendForm_), "%s", formBody);

  result = requestPost(formBody);
  if (result != ZteResult::kSuccess) {
    return result;
  }
  if (bodyLength_ == 0) {
    // The B02 firmware answers some malformed SEND_SMS forms with 200 and
    // an empty body; name it explicitly so hardware logs identify it.
    fail("send_reply_empty");
    return ZteResult::kSendRejected;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char value[16];
  if (!jsonMemberString(body, "result", value, sizeof(value)) || strcmp(value, "success") != 0) {
    fail("send");
    return ZteResult::kSendRejected;
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_sendSms

// #region FUNC_ZteModem_readSendStatus
// PURPOSE: Reads one sampled outcome of the modem's asynchronous send
// command (sms_cmd=4): "3" completed, "2" failed, anything else (including
// the field's absence once the command clears) counts as still in progress,
// so the caller's bounded wait is the only timeout source.
ZteResult ZteModem::readSendStatus(ZteSendStatus& out) {
  out = ZteSendStatus::kInProgress;
  ZteResult result = requestGet("isTest=false&cmd=sms_cmd_status_info&sms_cmd=4");
  if (result != ZteResult::kSuccess) {
    return result;
  }
  JsonView body{scratch_, scratch_ + bodyLength_};
  char value[8];
  if (!jsonMemberString(body, "sms_cmd_status_result", value, sizeof(value))) {
    return ZteResult::kSuccess;
  }
  if (strcmp(value, "3") == 0) {
    out = ZteSendStatus::kDone;
  } else if (strcmp(value, "2") == 0) {
    fail("send_status");
    out = ZteSendStatus::kFailed;
  }
  return ZteResult::kSuccess;
}
// #endregion FUNC_ZteModem_readSendStatus
