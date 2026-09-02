// #region MODULE_CONTRACT
// PURPOSE: Keeps SMTP delivery sequencing testable without a live socket.
// SCOPE:
// - Parses SMTP replies and drives bounded authentication and message-delivery
//   dialogs over SmtpChannel.
// - NOT: Socket transport ownership, persisted configuration, email composition,
//   and task scheduling.
// INVARIANTS:
// - Every exit path stops the channel;
// - credentials and body content never appear in error classification or stage reports;
// - every failure is traceable to one stage and the last SMTP reply code.
// #endregion MODULE_CONTRACT

#include "smtp/smtp_client.h"

#include <stdio.h>
#include <string.h>

#include "codec/codec_base64.h"

namespace {

constexpr size_t kLineBufferLength = 256;
constexpr size_t kExtensionsBufferLength = 512;
constexpr size_t kBase64LineLength = 76;
constexpr size_t kBase64ChunkBytes = (kBase64LineLength / 4) * 3;  // 57 bytes -> 76 chars

bool writeLine(SmtpChannel& channel, const char* line) {
  const size_t length = strlen(line);
  if (!channel.write(line, length)) {
    return false;
  }
  return channel.write("\r\n", 2);
}

// #region FUNC_readReplyLine
// PURPOSE: Rejects malformed server replies before they steer SMTP state.
int readReplyLine(SmtpChannel& channel, char* line, size_t lineSize, char* separator) {
  const int length = channel.readLine(line, lineSize);
  if (length < 0 || static_cast<size_t>(length) >= lineSize) {
    return -1;
  }
  line[length] = '\0';
  if (length < 3 || line[0] < '0' || line[0] > '9' || line[1] < '0' || line[1] > '9' ||
      line[2] < '0' || line[2] > '9') {
    return -1;
  }
  *separator = length == 3 ? '\0' : line[3];
  return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}
// #endregion FUNC_readReplyLine

// #region FUNC_readReply
// PURPOSE: Consumes a complete multi-line reply and collects the extension
// text of every line after the separator, joined with newlines.
int readReply(SmtpChannel& channel, char* extensions, size_t extensionsSize) {
  char line[kLineBufferLength];
  size_t used = 0;
  extensions[0] = '\0';
  for (;;) {
    char separator = '\0';
    const int code = readReplyLine(channel, line, sizeof(line), &separator);
    if (code < 0) {
      return -1;
    }
    const char* text = separator == '\0' ? line + 3 : line + 4;
    const size_t textLength = strlen(text);
    if (used + textLength + 1 < extensionsSize) {
      if (used > 0) {
        extensions[used++] = '\n';
      }
      memcpy(extensions + used, text, textLength);
      used += textLength;
      extensions[used] = '\0';
    }
    if (separator != '-') {
      return code;
    }
  }
}
// #endregion FUNC_readReply

// #region FUNC_equalsIgnoreCase
// PURPOSE: Compares one word of the EHLO extension list case-insensitively.
bool equalsIgnoreCase(const char* word, size_t wordLength, const char* expected) {
  const size_t expectedLength = strlen(expected);
  if (wordLength != expectedLength) {
    return false;
  }
  for (size_t index = 0; index < expectedLength; ++index) {
    char left = word[index];
    char right = expected[index];
    if (left >= 'a' && left <= 'z') {
      left = static_cast<char>(left - 'a' + 'A');
    }
    if (right >= 'a' && right <= 'z') {
      right = static_cast<char>(right - 'a' + 'A');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}
// #endregion FUNC_equalsIgnoreCase

// #region FUNC_hasExtension
// PURPOSE: Keeps TLS negotiation dependent on an explicit server capability.
bool hasExtension(const char* extensions, const char* keyword) {
  size_t start = 0;
  const size_t total = strlen(extensions);
  while (start <= total) {
    size_t end = start;
    while (extensions[end] != '\0' && extensions[end] != '\n') {
      ++end;
    }
    size_t wordEnd = start;
    while (wordEnd < end && extensions[wordEnd] != ' ') {
      ++wordEnd;
    }
    if (equalsIgnoreCase(extensions + start, wordEnd - start, keyword)) {
      return true;
    }
    if (extensions[end] == '\0') {
      break;
    }
    start = end + 1;
  }
  return false;
}
// #endregion FUNC_hasExtension

// #region FUNC_writeBase64Line
// PURPOSE: Encodes one chunk of at most 57 bytes and writes it as a wrapped
// base64 line, so bodies never need a second full-size buffer.
bool writeBase64Line(SmtpChannel& channel, const char* body, size_t remaining) {
  char encoded[kBase64LineLength + 3];
  size_t used = 0;
  while (remaining > 0 && used + 5 <= sizeof(encoded)) {
    const size_t chunk = remaining < 3 ? remaining : 3;
    codec::encodeBase64Chunk(reinterpret_cast<const unsigned char*>(body), chunk, encoded + used);
    used += 4;
    body += chunk;
    remaining -= chunk;
  }
  encoded[used++] = '\r';
  encoded[used++] = '\n';
  return channel.write(encoded, used);
}
// #endregion FUNC_writeBase64Line

// #region FUNC_writeBase64Body
// PURPOSE: Streams the whole body as 76-column base64 lines.
bool writeBase64Body(SmtpChannel& channel, const char* utf8Body) {
  size_t remaining = strlen(utf8Body);
  while (remaining > 0) {
    const size_t take = remaining < kBase64ChunkBytes ? remaining : kBase64ChunkBytes;
    if (!writeBase64Line(channel, utf8Body, take)) {
      return false;
    }
    utf8Body += take;
    remaining -= take;
  }
  return true;
}
// #endregion FUNC_writeBase64Body

// #region FUNC_writeAuthCredential
// PURPOSE: Keeps SMTP credentials encoded and bounded during authentication.
bool writeAuthCredential(SmtpChannel& channel, const char* credential) {
  char encoded[((kMaxSmtpUserLength / 3) + 2) * 4 + 3];
  const size_t used =
      codec::encodeBase64(credential, strlen(credential), encoded, sizeof(encoded) - 2);
  if (used == 0) {
    return false;
  }
  encoded[used] = '\r';
  encoded[used + 1] = '\n';
  return channel.write(encoded, used + 2);
}
// #endregion FUNC_writeAuthCredential

}  // namespace

// #region METHOD_SmtpClient_SmtpClient
// PURPOSE: Gives each submission one isolated transport lifetime.
SmtpClient::SmtpClient(SmtpChannel& channel) : channel_(channel) {}
// #endregion METHOD_SmtpClient_SmtpClient

void SmtpClient::fail(const char* stage, int code) {
  failedStage_ = stage;
  lastReplyCode_ = code;
}

void SmtpClient::reportStage(const char* stage, int code) {
  if (stageListener_ != nullptr) {
    stageListener_(stage, code);
  }
}

// #region METHOD_SmtpClient_runDialog
// PURPOSE: Executes the full submission sequence; every failure is classified
// and traceable to one stage plus the server's reply code.
SmtpSendResult SmtpClient::runDialog(const SmtpConfigRecord& config, const char* ehloName,
                                     const char* subject, const char* utf8Body) {
  char extensions[kExtensionsBufferLength];
  char command[kLineBufferLength];

  const int banner = readReply(channel_, extensions, sizeof(extensions));
  reportStage("banner", banner);
  if (banner != 220) {
    fail("banner", banner < 0 ? 0 : banner);
    return SmtpSendResult::kDialogFailed;
  }

  snprintf(command, sizeof(command), "EHLO %s", ehloName);
  if (!writeLine(channel_, command)) {
    fail("ehlo", 0);
    return SmtpSendResult::kDialogFailed;
  }
  int code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("ehlo", code);
  if (code != 250) {
    fail("ehlo", code < 0 ? 0 : code);
    return SmtpSendResult::kDialogFailed;
  }

  const bool startTlsMode =
      config.securityMode == static_cast<uint8_t>(SmtpSecurityMode::kStartTls);
  if (startTlsMode) {
    if (!hasExtension(extensions, "STARTTLS")) {
      fail("starttls_missing", code);
      return SmtpSendResult::kTlsUnavailable;
    }
    if (!writeLine(channel_, "STARTTLS")) {
      fail("starttls", 0);
      return SmtpSendResult::kDialogFailed;
    }
    code = readReply(channel_, extensions, sizeof(extensions));
    reportStage("starttls", code);
    if (code != 220) {
      fail("starttls", code < 0 ? 0 : code);
      return SmtpSendResult::kTlsUnavailable;
    }
    if (!channel_.startTls()) {
      fail("starttls_handshake", 0);
      return SmtpSendResult::kTlsFailed;
    }
    if (!writeLine(channel_, command)) {
      fail("ehlo_tls", 0);
      return SmtpSendResult::kDialogFailed;
    }
    code = readReply(channel_, extensions, sizeof(extensions));
    reportStage("ehlo_tls", code);
    if (code != 250) {
      fail("ehlo_tls", code < 0 ? 0 : code);
      return SmtpSendResult::kDialogFailed;
    }
  }

  if (!hasExtension(extensions, "AUTH")) {
    fail("auth_missing", 0);
    return SmtpSendResult::kDialogFailed;
  }
  if (!writeLine(channel_, "AUTH LOGIN")) {
    fail("auth_login", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("auth_login", code);
  if (code != 334) {
    fail("auth_login", code < 0 ? 0 : code);
    return SmtpSendResult::kAuthRejected;
  }
  if (!writeAuthCredential(channel_, config.username)) {
    fail("auth_username", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("auth_username", code);
  if (code != 334) {
    fail("auth_username", code < 0 ? 0 : code);
    return SmtpSendResult::kAuthRejected;
  }
  if (!writeAuthCredential(channel_, config.password)) {
    fail("auth_password", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("auth_password", code);
  if (code != 235) {
    fail("auth_password", code < 0 ? 0 : code);
    return SmtpSendResult::kAuthRejected;
  }

  snprintf(command, sizeof(command), "MAIL FROM:<%s>", config.fromAddress);
  if (!writeLine(channel_, command)) {
    fail("mail_from", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("mail_from", code);
  if (code != 250) {
    fail("mail_from", code < 0 ? 0 : code);
    return SmtpSendResult::kMessageRejected;
  }

  snprintf(command, sizeof(command), "RCPT TO:<%s>", config.recipientAddress);
  if (!writeLine(channel_, command)) {
    fail("rcpt_to", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("rcpt_to", code);
  if (code != 250 && code != 251) {
    fail("rcpt_to", code < 0 ? 0 : code);
    return SmtpSendResult::kMessageRejected;
  }

  if (!writeLine(channel_, "DATA")) {
    fail("data", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("data", code);
  if (code != 354) {
    fail("data", code < 0 ? 0 : code);
    return SmtpSendResult::kMessageRejected;
  }

  if (!writeLine(channel_, "MIME-Version: 1.0") ||
      !writeLine(channel_, "Content-Type: text/plain; charset=utf-8") ||
      !writeLine(channel_, "Content-Transfer-Encoding: base64")) {
    fail("headers", 0);
    return SmtpSendResult::kDialogFailed;
  }
  snprintf(command, sizeof(command), "From: <%s>", config.fromAddress);
  if (!writeLine(channel_, command)) {
    fail("headers", 0);
    return SmtpSendResult::kDialogFailed;
  }
  snprintf(command, sizeof(command), "To: <%s>", config.recipientAddress);
  if (!writeLine(channel_, command)) {
    fail("headers", 0);
    return SmtpSendResult::kDialogFailed;
  }
  snprintf(command, sizeof(command), "Subject: %s", subject);
  if (!writeLine(channel_, command)) {
    fail("headers", 0);
    return SmtpSendResult::kDialogFailed;
  }
  if (!channel_.write("\r\n", 2) || !writeBase64Body(channel_, utf8Body)) {
    fail("body", 0);
    return SmtpSendResult::kDialogFailed;
  }
  if (!channel_.write(".\r\n", 3)) {
    fail("dot", 0);
    return SmtpSendResult::kDialogFailed;
  }
  code = readReply(channel_, extensions, sizeof(extensions));
  reportStage("dot", code);
  if (code != 250) {
    fail("dot", code < 0 ? 0 : code);
    return SmtpSendResult::kMessageRejected;
  }

  writeLine(channel_, "QUIT");
  readReply(channel_, extensions, sizeof(extensions));
  return SmtpSendResult::kSuccess;
}
// #endregion METHOD_SmtpClient_runDialog

// #region METHOD_SmtpClient_sendMail
// PURPOSE: Runs one submission and guarantees the channel is closed afterwards
// so heap is released before the caller logs the outcome.
SmtpSendResult SmtpClient::sendMail(const SmtpConfigRecord& config, const char* ehloName,
                                    const char* subject, const char* utf8Body) {
  if (!channel_.connect(
          config.host, config.port,
          config.securityMode == static_cast<uint8_t>(SmtpSecurityMode::kImplicitTls))) {
    fail("connect", 0);
    return SmtpSendResult::kConnectFailed;
  }
  const SmtpSendResult result = runDialog(config, ehloName, subject, utf8Body);
  channel_.stop();
  return result;
}
// #endregion METHOD_SmtpClient_sendMail
