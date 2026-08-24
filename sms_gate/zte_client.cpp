// #region MODULE_CONTRACT
// PURPOSE: Implements the host-testable ZTE MF79RU goform dialog over any
// ZteChannel, reproducing the hardware-proven request sequence of the
// reference forwarder (see ADR-0003 and RESEARCH notes in the forwarder
// project): one HTTP/1.1 request per command, mandatory Referer, stok
// cookie session, AD = md5(md5(cr_version+wa_inner_version) + RD), and a
// lenient JSON scanner because the B02 firmware emits raw control
// characters inside string values.
// INVARIANTS: Every exit path stops the channel; credentials never appear
// in stage names or error paths; every failure is traceable to one stage;
// paging stays bounded by kZteMaxPages.
// #endregion MODULE_CONTRACT

#include "zte_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// then reads the response into the scratch buffer.
ZteResult ZteModem::requestPost(const char* formBody) {
  char request[768];
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

// #region FUNC_ZteModem_fetchSmsPage
// PURPOSE: Requests one inbox page into the scratch buffer, relogging in
// once when the modem answers the stale empty shape.
ZteResult ZteModem::fetchSmsPage(unsigned int page) {
  char query[192];
  snprintf(query, sizeof(query),
           "isTest=false&cmd=sms_data_total&page=%u&data_per_page=%u&mem_store=1&tags=10&"
           "order_by=order+by+id+asc",
           page, static_cast<unsigned>(kZtePageSize));
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
    ZteResult result = fetchSmsPage(page);
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

// #region FUNC_ZteModem_verifyAbsent
// PURPOSE: Confirms a deleted ID no longer appears, stopping as soon as the
// scan passes the ID's position in the effective ordering.
ZteResult ZteModem::verifyAbsent(const char* targetId) {
  uint32_t target = 0;
  if (!parseUint32String(targetId, target)) {
    fail("delete_verify");
    return ZteResult::kProtocolError;
  }
  bool ascendingDetected = false;
  bool ascending = true;
  for (unsigned int page = 0; page < kZteMaxPages; ++page) {
    ZteResult result = fetchSmsPage(page);
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
      if (idValue == target) {
        fail("delete_unverified");
        return ZteResult::kProtocolError;
      }
      if (ascendingDetected &&
          ((ascending && idValue > target) || (!ascending && idValue < target))) {
        return ZteResult::kSuccess;  // Passed the target's position.
      }
    }
    if (count < kZtePageSize) {
      return ZteResult::kSuccess;  // Final page reached.
    }
  }
  return ZteResult::kSuccess;  // Page cap reached; absence is the safe read.
}
// #endregion FUNC_ZteModem_verifyAbsent

// #region FUNC_ZteModem_deleteSms
// PURPOSE: Deletes exactly one message with a fresh AD token and verifies
// the ID disappeared so a silently failed delete cannot re-forward forever.
ZteResult ZteModem::deleteSms(const ZteSms& sms) {
  if (!hasSession()) {
    ZteResult result = openSession();
    if (result != ZteResult::kSuccess) {
      return result;
    }
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
  char ad[33];
  codec::md5Hex(concatenated, strlen(concatenated), ad);
  char formBody[192];
  snprintf(formBody, sizeof(formBody),
           "isTest=false&goformId=DELETE_SMS&msg_id=%s%%3B&notCallback=true&AD=%s", sms.id, ad);
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
  return verifyAbsent(sms.id);
}
// #endregion FUNC_ZteModem_deleteSms

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
