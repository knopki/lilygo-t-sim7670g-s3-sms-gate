// #region MODULE_CONTRACT
// PURPOSE: Renders one incoming SMS (ZTE MF79RU or SIM7670G) into the
// shared email subject and body shape so both sources produce the same
// operator-facing message with alias, INCOMPLETE flag and modem date.
// #endregion MODULE_CONTRACT

#include "system/email_builder.h"

#include "modem/modem_client.h"
#include "zte/zte_client.h"

// #region FUNC_sanitizeSenderForSubject
// PURPOSE: Reduces a sender field to printable ASCII so it can travel in
// an SMTP subject safely; control bytes become spaces, non-ASCII becomes
// '?', empty input maps to "unknown sender" and the result is capped at
// 40 characters.
String sanitizeSenderForSubject(const char* sender) {
  String clean;
  if (sender == nullptr) {
    return F("unknown sender");
  }
  clean.reserve(strlen(sender));
  for (const char* p = sender; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 32 || ch > 126) {
      clean += ch < 32 ? ' ' : '?';
    } else {
      clean += *p;
    }
  }
  clean.trim();
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
    const char* rec = (received != nullptr && received[0] != '\0') ? received : "?";
    const char* tot = (total != nullptr && total[0] != '\0') ? total : "?";
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
