// #region MODULE_CONTRACT
// PURPOSE: Implements the lenient JSON scanner for the ZTE MF79RU goform
// dialog, matching the B02 firmware's raw-control behaviour and the fixed
// paging shapes used by the inbox and send-status paths.
// #endregion MODULE_CONTRACT

#include "zte/zte_json.h"

#include <string.h>

#include "codec.h"
#include "zte/zte_client.h"

namespace {
constexpr uint32_t kInvalidHex = 0x200000;
}

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
// characters inside it; returns nullptr when the string never terminates.
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
        uint32_t codepoint = kInvalidHex;
        if (p + 4 <= value.end) {
          codepoint = codec::parseHex4(p);
        }
        if (codepoint <= 0x10FFFF) {
          p += 4;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF && p + 6 <= value.end && p[0] == '\\' &&
              p[1] == 'u') {
            const uint32_t low = codec::parseHex4(p + 2);
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
        codec::appendUtf8(codepoint, out, outSize, used);
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
  return false;
}
// #endregion FUNC_jsonMemberString

// #region CLASS_JsonArrayIterator_next
// PURPOSE: Steps to the next array element, bounding it as a JsonView.
bool JsonArrayIterator::next(JsonView& element) {
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
    return false;
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
// #endregion CLASS_JsonArrayIterator_next

// #region FUNC_jsonMemberArray
// PURPOSE: Bounds the value of one member when it is an array.
bool jsonMemberArray(JsonView object, const char* key, JsonView& array) {
  return findMember(object, key, array) && array.start < array.end && *array.start == '[';
}
// #endregion FUNC_jsonMemberArray

// #region FUNC_parseUint32String
// PURPOSE: Converts a digits-only decimal string to uint32 for ordering and
// reporting decisions.
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

// #region FUNC_copyRawView
// PURPOSE: Copies a non-hex content value verbatim, truncating at outSize.
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

// #region FUNC_isUcs2HexView
// PURPOSE: Validates the whole content view as 4-hex-per-codepoint UCS-2
// via the shared codec helper.
bool isUcs2HexView(JsonView view) {
  return codec::isUcs2HexView(view.start, static_cast<size_t>(view.end - view.start));
}
// #endregion FUNC_isUcs2HexView

// #region FUNC_decodeUcs2HexView
// PURPOSE: Decodes validated UCS-2 hex into UTF-8 via the shared codec helper.
size_t decodeUcs2HexView(JsonView view, char* out, size_t outSize) {
  return codec::decodeUcs2HexView(view.start, static_cast<size_t>(view.end - view.start), out,
                                  outSize);
}
// #endregion FUNC_decodeUcs2HexView

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
// PURPOSE: Reads the id and tag one scan step needs (tag optional).
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
