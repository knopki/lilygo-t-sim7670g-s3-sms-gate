// #region MODULE_CONTRACT
// PURPOSE: Gives every SMS source one consistent operator-facing email shape.
// SCOPE:
// - Renders SMS metadata and content into shared SMTP subjects and bodies.
// - NOT: Reading modem messages or sending email.
// INVARIANTS:
// - Dynamic subject text is printable ASCII; sender text is capped at 40 characters.
// - ZTE and SIM7670G messages use the same subject/body layout.
// #endregion MODULE_CONTRACT

#include "system/email_builder.h"

#include "modem/modem_client.h"
#include "zte/zte_client.h"

namespace {

// #region FUNC_sanitizeSubjectText
// PURPOSE: Removes control and non-ASCII bytes from dynamic SMTP subject text.
String sanitizeSubjectText(const char* value) {
  String clean;
  if (value == nullptr) {
    return clean;
  }
  clean.reserve(strlen(value));
  for (const char* p = value; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 32 || ch > 126) {
      clean += ch < 32 ? ' ' : '?';
    } else {
      clean += *p;
    }
  }
  clean.trim();
  return clean;
}
// #endregion FUNC_sanitizeSubjectText

}  // namespace

// #region FUNC_sanitizeSenderForSubject
// PURPOSE: Reduces a sender field to printable ASCII so it can travel in
// an SMTP subject safely; control bytes become spaces, non-ASCII becomes
// '?', empty input maps to "unknown sender" and the result is capped at
// 40 characters.
String sanitizeSenderForSubject(const char* sender) {
  const String clean = sanitizeSubjectText(sender);
  if (clean.length() == 0) {
    return F("unknown sender");
  }
  return clean.substring(0, 40);
}
// #endregion FUNC_sanitizeSenderForSubject

// #region FUNC_buildSmsEmailFromParts
// PURPOSE: Shared subject/body renderer for every SMS source so ZTE and
// SIM7670G share one email layout (alias prefix in the subject, alias via
// Received on:, INCOMPLETE tag and modem date formatting).
void buildSmsEmailFromParts(const char* senderRaw, const String& label, const char* id,
                            const char* dateRaw, const char* text, bool concatComplete,
                            const char* received, const char* total, String& subject,
                            String& body) {
  const String sender = sanitizeSenderForSubject(senderRaw != nullptr ? senderRaw : "");
  if (!concatComplete) {
    const String receivedClean = sanitizeSubjectText(received);
    const String totalClean = sanitizeSubjectText(total);
    const String rec = receivedClean.length() > 0 ? receivedClean : String(F("?"));
    const String tot = totalClean.length() > 0 ? totalClean : String(F("?"));
    subject = F("[INCOMPLETE ");
    subject += rec;
    subject += '/';
    subject += tot;
    subject += F("] SMS from ");
    subject += sender;
  } else {
    subject = F("SMS from ");
    subject += sender;
  }
  if (label.length() > 0) {
    String prefixed;
    prefixed.reserve(label.length() + 2 + subject.length());
    prefixed += '[';
    prefixed += label;
    prefixed += F("] ");
    prefixed += subject;
    subject = prefixed;
  }
  body = F("Sender: ");
  body += sender;
  if (label.length() > 0) {
    body += F("\nReceived on: ");
    body += label;
  }
  body += F("\nModem message ID: ");
  body += id != nullptr ? id : "";
  body += F("\nModem date: ");
  char formatted[64];
  formatZteDate(dateRaw != nullptr ? dateRaw : "", formatted, sizeof(formatted));
  body += formatted;
  body += F("\n\n");
  if (!concatComplete) {
    body +=
        F("WARNING: modem received only part of this concatenated SMS; "
          "this email contains the available fragment.\n\n");
  }
  body += text != nullptr ? text : "";
}
// #endregion FUNC_buildSmsEmailFromParts

// #region FUNC_buildZteSmsEmail
// PURPOSE: Renders one ZteSms via the shared helper using the ZTE label
// alias so the modem-source and ZTE paths stay layout-identical.
void buildZteSmsEmail(const ZteSms& sms, const String& label, String& subject, String& body) {
  buildSmsEmailFromParts(sms.number, label, sms.id, sms.dateRaw, sms.textUtf8, sms.concatComplete,
                         sms.concatReceived, sms.concatTotal, subject, body);
}
// #endregion FUNC_buildZteSmsEmail

// #region FUNC_buildModemSmsEmail
// PURPOSE: Renders one ModemSms via the shared helper using the modem
// source label alias.
void buildModemSmsEmail(const ModemSms& sms, const String& label, String& subject, String& body) {
  buildSmsEmailFromParts(sms.number, label, sms.id, sms.date, sms.text, sms.concatComplete,
                         sms.concatReceived, sms.concatTotal, subject, body);
}
// #endregion FUNC_buildModemSmsEmail
