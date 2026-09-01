// #region MODULE_CONTRACT
// PURPOSE: Extracts bounded ZTE data despite firmware-specific JSON.
// SCOPE:
// - Parses bounded ZTE JSON responses and decodes their modem-specific values.
// - NOT: Performing ZTE transport requests or managing polling lifecycle.
// INVARIANTS:
// - Every scan remains within the supplied response bounds.
// - Malformed JSON and unsupported values fail without producing unbounded output.
// #endregion MODULE_CONTRACT

#include "zte/zte_json.h"

#include <stdint.h>
#include <string.h>

#include "codec.h"
#include "zte/zte_client.h"

namespace {
constexpr uint32_t kInvalidHex = 0x200000;
}

// #region FUNC_skipWhitespace
// PURPOSE: Keeps bounded scans independent of formatting whitespace.
const char* skipWhitespace(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
    ++p;
  }
  return p;
}
// #endregion FUNC_skipWhitespace

// #region FUNC_skipString
// PURPOSE: Stops malformed strings from making later scans escape the response.
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
// PURPOSE: Lets unknown JSON values be skipped without treating firmware extensions as errors.
bool isLiteralCharacter(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         ch == '-' || ch == '+' || ch == '.';
}
// #endregion FUNC_isLiteralCharacter

// #region FUNC_skipValue
// PURPOSE: Preserves forward compatibility while keeping every skipped value bounded.
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
// PURPOSE: Makes selected text fields safe for forwarding and bounded output.
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

// #region METHOD_JsonArrayIterator_next
// PURPOSE: Lets paging scans consume arrays without crossing element bounds.
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
// #endregion METHOD_JsonArrayIterator_next

// #region FUNC_jsonMemberArray
// PURPOSE: Lets callers inspect selected arrays without copying or overreading them.
bool jsonMemberArray(JsonView object, const char* key, JsonView& array) {
  return findMember(object, key, array) && array.start < array.end && *array.start == '[';
}
// #endregion FUNC_jsonMemberArray

// #region FUNC_parseUint32String
// PURPOSE: Keeps ordering and reporting decisions safe from malformed IDs.
bool parseUint32String(const char* id, uint32_t& out) {
  if (*id == '\0') {
    return false;
  }
  uint32_t value = 0;
  for (const char* p = id; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > UINT32_MAX / 10 || (value == UINT32_MAX / 10 && digit > UINT32_MAX % 10)) {
      return false;
    }
    value = value * 10 + digit;
  }
  out = value;
  return true;
}
// #endregion FUNC_parseUint32String

// #region FUNC_copyRawView
// PURPOSE: Keeps unknown message content forwardable without overrunning output.
void copyRawView(JsonView view, char* out, size_t outSize) {
  size_t used = 0;
  for (const char* p = view.start; p < view.end && used + 1 < outSize; ++p) {
    out[used++] = *p;
  }
  out[used] = '\0';
}
// #endregion FUNC_copyRawView

// #region FUNC_innerStringValue
// PURPOSE: Gives content decoders a bounded view of raw string bytes.
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
// PURPOSE: Keeps decoding on the UCS-2 path only when content is wholly valid.
bool isUcs2HexView(JsonView view) {
  return codec::isUcs2HexView(view.start, static_cast<size_t>(view.end - view.start));
}
// #endregion FUNC_isUcs2HexView

// #region FUNC_decodeUcs2HexView
// PURPOSE: Makes valid encoded content usable by forwarding without extra storage.
size_t decodeUcs2HexView(JsonView view, char* out, size_t outSize) {
  return codec::decodeUcs2HexView(view.start, static_cast<size_t>(view.end - view.start), out,
                                  outSize);
}
// #endregion FUNC_decodeUcs2HexView

// #region FUNC_captureMessage
// PURPOSE: Keeps modem-specific field extraction out of forwarding logic.
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
// PURPOSE: Gives cleanup only bounded ID and tag metadata for one record.
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
// PURPOSE: Keeps framing checks limited to the headers this dialog trusts.
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
// PURPOSE: Rejects untrusted body lengths before they control reads.
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
