// #region MODULE_CONTRACT
// PURPOSE: Prevents ZTE form values from changing meaning on the wire.
// SCOPE:
// - Percent-escaping and bounded assembly of goform values.
// - NOT: HTTP framing, JSON scanning, or command sequencing.
// INVARIANTS: Output is always terminated; overflow fails before writing.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_FORM_CODEC_H
#define ZTE_ZTE_FORM_CODEC_H

#include <stddef.h>

// #region FUNC_isUnreservedFormByte
// PURPOSE: Keeps delimiters from changing form-field meaning.
bool isUnreservedFormByte(unsigned char ch);
// #endregion FUNC_isUnreservedFormByte

// #region FUNC_appendFormEscaped
// PURPOSE: Keeps arbitrary values safe inside bounded form bodies.
bool appendFormEscaped(const char* value, char* out, size_t outSize, size_t& used);
// #endregion FUNC_appendFormEscaped

// #region FUNC_appendLiteral
// PURPOSE: Completes form bodies without risking unterminated output.
bool appendLiteral(const char* literal, char* out, size_t outSize, size_t& used);
// #endregion FUNC_appendLiteral
#endif  // ZTE_ZTE_FORM_CODEC_H
