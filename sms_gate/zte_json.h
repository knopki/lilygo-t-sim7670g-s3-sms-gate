// #region MODULE_CONTRACT
// PURPOSE: Provides the lenient JSON scanner used by the ZTE MF79RU goform
// dialog so the B02 firmware's raw control characters inside strings and the
// fixed-shape inbox paging never need a full JSON library.
// SCOPE:
// - Bounds values with JsonView, skips whitespace/strings/values, finds
// members, decodes string members (including \u escapes and surrogates),
// iterates arrays, parses uint32 decimals, validates UCS-2 hex and decodes
// it, captures one SMS object, parses entry headers and HTTP headers.
// - NOT: HTTP transport, goform AD token, NVS, SMTP, or HTTP routes.
// INVARIANTS: The scanner never copies the response buffer, always stays
// within the bounded view, accepts raw control bytes inside strings, and
// never logs credentials.
// DEPENDENCIES: Pure C++ (codec.h for base64/MD5/UCS2 helpers, zte_client.h
// for ZteSms when captureMessage is used).
// #endregion MODULE_CONTRACT

#pragma once

#include <stddef.h>
#include <stdint.h>

// Forward declaration to avoid circular include with zte_client.h.
struct ZteSms;

// #region STRUCT_JsonView
// PURPOSE: Bounds one JSON value inside the response scratch buffer so the
// scanner never needs copies or termination of the scanned region.
struct JsonView {
  const char* start;
  const char* end;
};
// #endregion STRUCT_JsonView

// #region CLASS_JsonArrayIterator
// PURPOSE: Walks one JSON array element by element so callers see bounded
// views without copying.
class JsonArrayIterator {
 public:
  explicit JsonArrayIterator(JsonView array) : cursor_(array.start), end_(array.end) {}

  bool next(JsonView& element);

 private:
  const char* cursor_;
  const char* end_;
};
// #endregion CLASS_JsonArrayIterator

// Scanner primitives.
const char* skipWhitespace(const char* p, const char* end);
const char* skipString(const char* p, const char* end);
bool isLiteralCharacter(char ch);
const char* skipValue(const char* p, const char* end);
bool findMember(JsonView object, const char* key, JsonView& value);

// High-level member decoders.
bool jsonMemberString(JsonView object, const char* key, char* out, size_t outSize);
bool jsonMemberArray(JsonView object, const char* key, JsonView& array);

// Scalar parsers and header helpers.
bool parseUint32String(const char* id, uint32_t& out);
bool headerHasPrefix(const char* line, const char* prefix);
bool parseContentLength(const char* value, size_t& out);

// SMS-specific helpers used by ZteModem.
void copyRawView(JsonView view, char* out, size_t outSize);
bool innerStringValue(JsonView value, JsonView& inner);
bool isUcs2HexView(JsonView view);
size_t decodeUcs2HexView(JsonView view, char* out, size_t outSize);
bool captureMessage(JsonView element, ZteSms& out);
bool parseEntryHeader(JsonView element, char* id, size_t idSize, char* tag, size_t tagSize);
