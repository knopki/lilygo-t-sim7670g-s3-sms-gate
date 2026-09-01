// #region MODULE_CONTRACT
// PURPOSE: Keeps firmware-specific JSON inside bounded, testable scans.
// SCOPE:
// - JSON views, member/array scanning, scalar/header parsing, and SMS decoding.
// - NOT: HTTP transport, goform sequencing, persistence, SMTP, or routes.
// INVARIANTS: Scans never copy or overrun the response view; output is bounded.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_JSON_H
#define ZTE_ZTE_JSON_H

#include <stddef.h>
#include <stdint.h>

// Forward declaration to avoid circular include with zte_client.h.
struct ZteSms;

// #region STRUCT_JsonView
// PURPOSE: Keeps scans inside the modem response without copying values.
struct JsonView {
  const char* start;
  const char* end;
};
// #endregion STRUCT_JsonView

// #region CLASS_JsonArrayIterator
// PURPOSE: Lets callers consume arrays incrementally without allocations.
class JsonArrayIterator {
 public:
  explicit JsonArrayIterator(JsonView array) : cursor_(array.start), end_(array.end) {}

  // #region METHOD_JsonArrayIterator_next
  // PURPOSE: Supplies the next element without losing array bounds.
  bool next(JsonView& element);
  // #endregion METHOD_JsonArrayIterator_next

 private:
  const char* cursor_;
  const char* end_;
};
// #endregion CLASS_JsonArrayIterator

// #region FUNC_skipWhitespace
// PURPOSE: Prevents whitespace handling from escaping the response view.
const char* skipWhitespace(const char* p, const char* end);
// #endregion FUNC_skipWhitespace

// #region FUNC_skipString
// PURPOSE: Lets malformed strings fail before scanning beyond the response.
const char* skipString(const char* p, const char* end);
// #endregion FUNC_skipString

// #region FUNC_isLiteralCharacter
// PURPOSE: Keeps unknown-value scanning limited to modem JSON syntax.
bool isLiteralCharacter(char ch);
// #endregion FUNC_isLiteralCharacter

// #region FUNC_skipValue
// PURPOSE: Keeps unknown firmware fields harmless without a full parser.
const char* skipValue(const char* p, const char* end);
// #endregion FUNC_skipValue

// #region FUNC_findMember
// PURPOSE: Exposes named fields without allocating a parsed tree.
bool findMember(JsonView object, const char* key, JsonView& value);
// #endregion FUNC_findMember

// #region FUNC_jsonMemberString
// PURPOSE: Makes selected fields usable while preserving output bounds.
bool jsonMemberString(JsonView object, const char* key, char* out, size_t outSize);
// #endregion FUNC_jsonMemberString

// #region FUNC_jsonMemberArray
// PURPOSE: Makes selected arrays consumable without copying their contents.
bool jsonMemberArray(JsonView object, const char* key, JsonView& array);
// #endregion FUNC_jsonMemberArray

// #region FUNC_parseUint32String
// PURPOSE: Keeps message ordering safe from malformed numeric IDs.
bool parseUint32String(const char* id, uint32_t& out);
// #endregion FUNC_parseUint32String

// #region FUNC_headerHasPrefix
// PURPOSE: Recognizes headers safely within line-oriented protocol data.
bool headerHasPrefix(const char* line, const char* prefix);
// #endregion FUNC_headerHasPrefix

// #region FUNC_parseContentLength
// PURPOSE: Prevents peer-provided body lengths from overrunning reads.
bool parseContentLength(const char* value, size_t& out);
// #endregion FUNC_parseContentLength

// #region FUNC_copyRawView
// PURPOSE: Preserves raw content without exposing unterminated output.
void copyRawView(JsonView view, char* out, size_t outSize);
// #endregion FUNC_copyRawView

// #region FUNC_innerStringValue
// PURPOSE: Lets decoders inspect string bytes without copying them.
bool innerStringValue(JsonView value, JsonView& inner);
// #endregion FUNC_innerStringValue

// #region FUNC_isUcs2HexView
// PURPOSE: Selects the correct SMS decoding path without guessing.
bool isUcs2HexView(JsonView view);
// #endregion FUNC_isUcs2HexView

// #region FUNC_decodeUcs2HexView
// PURPOSE: Makes encoded SMS content usable by the forwarding pipeline.
size_t decodeUcs2HexView(JsonView view, char* out, size_t outSize);
// #endregion FUNC_decodeUcs2HexView

// #region FUNC_captureMessage
// PURPOSE: Keeps firmware-specific message fields out of forwarding logic.
bool captureMessage(JsonView element, ZteSms& out);
// #endregion FUNC_captureMessage

// #region FUNC_parseEntryHeader
// PURPOSE: Gives cleanup only the metadata needed to target one record.
bool parseEntryHeader(JsonView element, char* id, size_t idSize, char* tag, size_t tagSize);
// #endregion FUNC_parseEntryHeader
#endif  // ZTE_ZTE_JSON_H
