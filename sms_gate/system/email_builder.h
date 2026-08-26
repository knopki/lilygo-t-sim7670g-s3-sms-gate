// #region MODULE_CONTRACT
// PURPOSE: Renders one incoming SMS (ZTE MF79RU or SIM7670G) into the
// shared email subject and body shape so both sources produce the same
// operator-facing message with alias, INCOMPLETE flag and modem date.
// SCOPE:
// - sanitizeSenderForSubject, buildSmsEmailFromParts, buildZteSmsEmail
//   and buildModemSmsEmail; the modem date formatter is reused from
//   zte_client (formatZteDate).
// - NOT: SMTP transport, NVS persistence, HTTP routing and modem dialogs.
// INVARIANTS: Control characters are never emitted in the subject; an
// incomplete concatenated SMS always carries [INCOMPLETE n/m] and a
// warning in the body; formatted dates fall back to the raw modem field
// when parsing fails.
// DEPENDENCIES: Uses Arduino String, zte_client::formatZteDate and
// sms_validate-free helpers; both ZteSms and ModemSms are carried by
// value reference without owning NVS state.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_EMAIL_BUILDER_H
#define SYSTEM_EMAIL_BUILDER_H

#include <Arduino.h>

struct ZteSms;
struct ModemSms;

String sanitizeSenderForSubject(const char* sender);
void buildSmsEmailFromParts(const char* senderRaw, const String& label, const char* id,
                            const char* dateRaw, const char* text, bool concatComplete,
                            const char* received, const char* total, String& subject, String& body);
void buildZteSmsEmail(const ZteSms& sms, const String& label, String& subject, String& body);
void buildModemSmsEmail(const ModemSms& sms, const String& label, String& subject, String& body);
#endif  // SYSTEM_EMAIL_BUILDER_H
