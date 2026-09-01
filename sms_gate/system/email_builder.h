// #region MODULE_CONTRACT
// PURPOSE: Gives every SMS source one consistent operator-facing email shape.
// SCOPE:
// - Sanitizes sender/date metadata and renders complete or incomplete
// ZTE/modem SMS.
// - NOT: SMTP, persistence, HTTP, or modem dialogs.
// INVARIANTS:
// - Subjects exclude control characters;
// - incomplete parts carry a visible warning;
// - invalid dates fall back to the raw modem field.
// DEPENDENCIES: Arduino String, ZteSms/ModemSms, formatZteDate.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_EMAIL_BUILDER_H
#define SYSTEM_EMAIL_BUILDER_H

#include <Arduino.h>

struct ZteSms;
struct ModemSms;

// #region FUNC_sanitizeSenderForSubject
// PURPOSE: Prevents modem sender data from corrupting the email subject.
String sanitizeSenderForSubject(const char* sender);
// #endregion FUNC_sanitizeSenderForSubject

// #region FUNC_buildSmsEmailFromParts
// PURPOSE: Keeps forwarded SMS layout identical across modem sources.
void buildSmsEmailFromParts(const char* senderRaw, const String& label, const char* id,
                            const char* dateRaw, const char* text, bool concatComplete,
                            const char* received, const char* total, String& subject, String& body);
// #endregion FUNC_buildSmsEmailFromParts

// #region FUNC_buildZteSmsEmail
// PURPOSE: Adapts one ZTE SMS to the shared email renderer.
void buildZteSmsEmail(const ZteSms& sms, const String& label, String& subject, String& body);
// #endregion FUNC_buildZteSmsEmail

// #region FUNC_buildModemSmsEmail
// PURPOSE: Adapts one SIM7670G SMS to the shared email renderer.
void buildModemSmsEmail(const ModemSms& sms, const String& label, String& subject, String& body);
// #endregion FUNC_buildModemSmsEmail
#endif  // SYSTEM_EMAIL_BUILDER_H
