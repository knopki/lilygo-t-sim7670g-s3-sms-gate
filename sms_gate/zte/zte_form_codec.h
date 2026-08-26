// #region MODULE_CONTRACT
// PURPOSE: Encodes application/x-www-form-urlencoded values for the ZTE
// MF79RU goform channel so SEND_SMS and other POST forms always carry the
// exact bytes the modem expects.
// SCOPE:
// - Percent-escapes every non-unreserved byte (including '+' and ';'), and
// appends fixed literals, both with bounded buffers and guaranteed
// termination.
// - NOT: HTTP framing, JSON scanning, or goform command sequencing.
// INVARIANTS: Every appended value is terminated; overflow always returns
// false so no stack bytes leak onto the wire or into Content-Length.
// DEPENDENCIES: Pure C++.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_FORM_CODEC_H
#define ZTE_ZTE_FORM_CODEC_H

#include <stddef.h>

bool isUnreservedFormByte(unsigned char ch);
bool appendFormEscaped(const char* value, char* out, size_t outSize, size_t& used);
bool appendLiteral(const char* literal, char* out, size_t outSize, size_t& used);
#endif  // ZTE_ZTE_FORM_CODEC_H
