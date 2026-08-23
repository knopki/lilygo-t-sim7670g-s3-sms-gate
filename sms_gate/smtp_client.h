// #region MODULE_CONTRACT
// PURPOSE: Runs the SMTP submission dialog (EHLO, STARTTLS, AUTH LOGIN, MAIL,
// DATA) over an abstract channel so the protocol logic stays host-testable.
// SCOPE:
// - One mail submission per sendMail call, base64 body encoding, multi-line
// reply parsing, STARTTLS upgrade ordering, and stage-level failure tracing.
// - NOT: Sockets, TLS handshakes, NVS persistence, and HTML rendering.
// INVARIANTS: No credential or message content is logged or formatted into
// error paths; the channel is always stopped before returning.
// DEPENDENCIES: Pure C++; the device channel lives in smtp_transport.h.
// #endregion MODULE_CONTRACT

#pragma once

#include <stddef.h>

#include "smtp_record.h"

// #region CLASS_SmtpChannel
// PURPOSE: Abstracts the byte transport and TLS upgrade point so tests can
// script a server and the device can bind NetworkClientSecure.
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
// PURPOSE: Gives the caller one stable outcome per failure class for Serial
// events and future retry decisions.
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

// #region FUNC_SmtpStageListener
// PURPOSE: Reports every completed protocol step (stage name and SMTP reply
// code, 0 when the step failed before any reply) so the device can trace a
// dialog without logging credentials or message content.
using SmtpStageListener = void (*)(const char* stage, int code);
// #endregion FUNC_SmtpStageListener

// #region CLASS_SmtpClient
// PURPOSE: Owns the protocol sequencing for one submission so callers only
// provide a channel and a validated record, and reports exactly which dialog
// stage failed and with which SMTP reply code.
class SmtpClient {
 public:
  explicit SmtpClient(SmtpChannel& channel);

  // Optional per-stage trace; fires once for every completed protocol step.
  void setStageListener(SmtpStageListener listener) { stageListener_ = listener; }

  // Sends one message. subject must be printable ASCII; utf8Body is
  // base64-encoded by the dialog itself. ehloName identifies the client.
  SmtpSendResult sendMail(const SmtpConfigRecord& config, const char* ehloName, const char* subject,
                          const char* utf8Body);

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
