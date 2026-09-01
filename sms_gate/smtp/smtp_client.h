// #region MODULE_CONTRACT
// PURPOSE: Keeps SMTP delivery sequencing testable without a live socket.
// SCOPE:
// - Submission dialog, reply parsing, TLS upgrade, and failure tracing.
// - NOT: Socket ownership, TLS implementation, persistence, or rendering.
// INVARIANTS:
// - Channels stop on return;
// - credentials and message content stay out of logs and error classification.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SMTP_SMTP_CLIENT_H
#define SMTP_SMTP_CLIENT_H

#include <stddef.h>

#include "smtp/smtp_record.h"

// #region CLASS_SmtpChannel
// PURPOSE: Keeps SMTP protocol tests independent from socket and TLS hardware.
class SmtpChannel {
 public:
  virtual ~SmtpChannel() = default;

  // Opens the transport. implicitTls selects TLS from the first byte;
  // otherwise the connection stays plain until startTls() succeeds.
  virtual bool connect(const char* host, uint16_t port, bool implicitTls) = 0;
  // Writes exactly length bytes or returns false.
  virtual bool write(const char* data, size_t length) = 0;
  // Reads one CRLF-terminated line without the terminator into buffer.
  // Returns the line length, or a negative value on timeout or failure.
  virtual int readLine(char* buffer, size_t size) = 0;
  // Upgrades the plain connection to TLS. Returns false on failure.
  virtual bool startTls() = 0;
  virtual void stop() = 0;
};
// #endregion CLASS_SmtpChannel

// #region ENUM_SmtpSendResult
// PURPOSE: Keeps delivery failures classifiable for logs and retries.
enum class SmtpSendResult {
  kSuccess,
  kConnectFailed,
  kTlsUnavailable,
  kTlsFailed,
  kAuthRejected,
  kMessageRejected,
  kDialogFailed,
};
// #endregion ENUM_SmtpSendResult

// #region FUNC_smtpSendResultName
// PURPOSE: Keeps SMTP result names consistent across services.
inline const char* smtpSendResultName(SmtpSendResult result) {
  switch (result) {
    case SmtpSendResult::kSuccess:
      return "success";
    case SmtpSendResult::kConnectFailed:
      return "connect_failed";
    case SmtpSendResult::kTlsUnavailable:
      return "tls_unavailable";
    case SmtpSendResult::kTlsFailed:
      return "tls_failed";
    case SmtpSendResult::kAuthRejected:
      return "auth_rejected";
    case SmtpSendResult::kMessageRejected:
      return "message_rejected";
    default:
      return "dialog_failed";
  }
}
// #endregion FUNC_smtpSendResultName

using SmtpStageListener = void (*)(const char* stage, int code);

// #region CLASS_SmtpClient
// PURPOSE: Keeps SMTP submission sequencing bounded and observable.
class SmtpClient {
 public:
  // #region METHOD_SmtpClient_SmtpClient
  // PURPOSE: Gives each submission one isolated transport lifetime.
  explicit SmtpClient(SmtpChannel& channel);
  // #endregion METHOD_SmtpClient_SmtpClient

  // Optional per-stage trace; fires once for every completed protocol step.
  void setStageListener(SmtpStageListener listener) { stageListener_ = listener; }

  // #region METHOD_SmtpClient_sendMail
  // PURPOSE: Keeps SMTP protocol sequencing out of service and route logic.
  SmtpSendResult sendMail(const SmtpConfigRecord& config, const char* ehloName, const char* subject,
                          const char* utf8Body);
  // #endregion METHOD_SmtpClient_sendMail

  // Stage at which the last sendMail attempt failed ("" on success).
  const char* failedStage() const { return failedStage_; }
  // SMTP reply code received at the failed stage; 0 when the stage failed
  // on a local write/timeout before any reply arrived.
  int lastReplyCode() const { return lastReplyCode_; }

 private:
  SmtpSendResult runDialog(const SmtpConfigRecord& config, const char* ehloName,
                           const char* subject, const char* utf8Body);
  void fail(const char* stage, int code);
  void reportStage(const char* stage, int code);

  SmtpChannel& channel_;
  SmtpStageListener stageListener_ = nullptr;
  const char* failedStage_ = "";
  int lastReplyCode_ = 0;
};
// #endregion CLASS_SmtpClient
#endif  // SMTP_SMTP_CLIENT_H
