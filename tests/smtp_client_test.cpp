// #region MODULE_CONTRACT
// PURPOSE: Locks SMTP dialog and record rules before firmware reaches a mail server.
// SCOPE:
// - Tests SMTP record validation and scripted TLS, authentication, message,
//   and rejection dialog outcomes.
// INVARIANTS:
// - STARTTLS credentials are sent only after a successful upgrade;
// - every attempted session closes its channel and reports its failure stage.
// #endregion MODULE_CONTRACT

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../sms_gate/smtp/smtp_client.h"
#include "../sms_gate/smtp/smtp_record.h"

namespace {

// #region FUNC_makeRecord
// PURPOSE: Supplies a known-good baseline so each mutation isolates one integrity rule.
SmtpConfigRecord makeRecord() {
  SmtpConfigRecord record{};
  record.magic = kSmtpConfigMagic;
  record.version = kSmtpConfigVersion;
  record.port = 587;
  record.securityMode = static_cast<uint8_t>(SmtpSecurityMode::kStartTls);
  strcpy(record.host, "smtp.example.com");
  strcpy(record.username, "user@example.com");
  strcpy(record.password, "secret-password-123");
  strcpy(record.fromAddress, "device@example.com");
  strcpy(record.recipientAddress, "owner@example.com");
  record.checksum = calculateSmtpConfigChecksum(record);
  return record;
}
// #endregion FUNC_makeRecord

// #region CLASS_FakeChannel
// PURPOSE: Keeps one deterministic server dialog so exact client writes remain testable.
class FakeChannel : public SmtpChannel {
 public:
  void enqueueReply(const char* reply) {
    strcat(replies_, reply);
    strcat(replies_, "\n");
  }

  void expectCommand(const char* command) {
    assert(expectedCount_ < kMaxExpected);
    expected_[expectedCount_++] = command;
  }

  bool connectCalled = false;
  bool connectImplicitTls = false;
  bool startTlsCalled = false;
  bool stopCalled = false;
  bool failStartTls = false;

  bool connect(const char* host, uint16_t port, bool implicitTls) override {
    assert(strcmp(host, "smtp.example.com") == 0);
    assert(port == 587 || port == 465);
    connectCalled = true;
    connectImplicitTls = implicitTls;
    return true;
  }

  bool write(const char* data, size_t length) override {
    assert(writtenLength_ + length < sizeof(written_));
    memcpy(written_ + writtenLength_, data, length);
    writtenLength_ += length;
    written_[writtenLength_] = '\0';
    return true;
  }

  int readLine(char* buffer, size_t size) override {
    assert(replies_[cursor_] != '\0');
    size_t used = 0;
    while (replies_[cursor_] != '\0' && replies_[cursor_] != '\n') {
      assert(used + 1 < size);
      buffer[used++] = replies_[cursor_++];
    }
    assert(replies_[cursor_] == '\n');
    ++cursor_;
    buffer[used] = '\0';
    return static_cast<int>(used);
  }

  bool startTls() override {
    startTlsCalled = true;
    return !failStartTls;
  }

  void stop() override { stopCalled = true; }

  const char* written() const { return written_; }
  bool stopCalledOnce() const { return stopCalled; }

  // Every expected command must appear in order as a complete line.
  bool expectationsMet() const {
    size_t searchFrom = 0;
    for (int index = 0; index < expectedCount_; ++index) {
      const char* found = findLine(expected_[index], searchFrom);
      if (found == nullptr) {
        fprintf(stderr, "missing or out-of-order command: %s\n", expected_[index]);
        return false;
      }
      searchFrom = static_cast<size_t>(found - written_) + strlen(expected_[index]) + 2;
    }
    return true;
  }

 private:
  // Returns a pointer to the start of a complete line equal to wanted.
  const char* findLine(const char* wanted, size_t from) const {
    const size_t wantedLength = strlen(wanted);
    size_t position = from;
    while (position + wantedLength + 2 <= writtenLength_ + 1) {
      const bool atStart = position == 0;
      const bool afterLine =
          position >= 2 && written_[position - 2] == '\r' && written_[position - 1] == '\n';
      if ((atStart || afterLine) && strncmp(written_ + position, wanted, wantedLength) == 0 &&
          written_[position + wantedLength] == '\r' &&
          written_[position + wantedLength + 1] == '\n') {
        return written_ + position;
      }
      ++position;
    }
    return nullptr;
  }

  static constexpr int kMaxExpected = 12;
  const char* expected_[kMaxExpected] = {};
  int expectedCount_ = 0;
  char replies_[2048] = {};
  size_t cursor_ = 0;
  char written_[4096] = {};
  size_t writtenLength_ = 0;
};
// #endregion CLASS_FakeChannel

// #region FUNC_testRecordValidation
// PURPOSE: Prevents invalid records from reaching SMTP setup.
void testRecordValidation() {
  SmtpConfigRecord record = makeRecord();
  assert(isSmtpConfigRecordValid(record));

  record.checksum += 1;
  assert(!isSmtpConfigRecordValid(record));
  record = makeRecord();
  record.magic = 0xdeadbeef;
  assert(!isSmtpConfigRecordValid(record));
  record = makeRecord();
  record.version = 99;
  assert(!isSmtpConfigRecordValid(record));

  record = makeRecord();
  memset(record.host, 'h', kMaxSmtpHostLength);
  record.host[kMaxSmtpHostLength] = '\0';
  memset(record.username, 'u', kMaxSmtpUserLength);
  record.username[kMaxSmtpUserLength] = '\0';
  memset(record.password, 'p', kMaxSmtpPasswordLength);
  record.password[kMaxSmtpPasswordLength] = '\0';
  memset(record.fromAddress, 'f', kMaxSmtpAddressLength);
  record.fromAddress[1] = '@';
  record.fromAddress[kMaxSmtpAddressLength] = '\0';
  memset(record.recipientAddress, 'r', kMaxSmtpAddressLength);
  record.recipientAddress[1] = '@';
  record.recipientAddress[kMaxSmtpAddressLength] = '\0';
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(isSmtpConfigRecordValid(record));

  record = makeRecord();
  record.host[0] = '\0';
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(!isSmtpConfigRecordValid(record));

  record = makeRecord();
  record.securityMode = 7;
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(!isSmtpConfigRecordValid(record));

  record = makeRecord();
  record.port = 0;
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(!isSmtpConfigRecordValid(record));

  record = makeRecord();
  record.fromAddress[4] = '@';
  record.fromAddress[5] = '\0';
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(isSmtpConfigRecordValid(record));
  record.fromAddress[4] = 'x';
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(!isSmtpConfigRecordValid(record));  // Missing '@'.

  record = makeRecord();
  record.password[0] = '\0';
  record.checksum = calculateSmtpConfigChecksum(record);
  assert(!isSmtpConfigRecordValid(record));

  printf("testRecordValidation ok\n");
}
// #endregion FUNC_testRecordValidation

// #region FUNC_enqueueEhlo
// PURPOSE: Keeps EHLO capability variants explicit so STARTTLS decisions remain testable.
void enqueueEhlo(FakeChannel& channel, bool withStartTls) {
  channel.enqueueReply("250-smtp.example.com at your service");
  channel.enqueueReply("250-SIZE 35882577");
  channel.enqueueReply("250-8BITMIME");
  if (withStartTls) {
    channel.enqueueReply("250-STARTTLS");
  }
  channel.enqueueReply("250-AUTH LOGIN PLAIN");
  channel.enqueueReply("250 ENHANCEDSTATUSCODES");
}
// #endregion FUNC_enqueueEhlo

// #region FUNC_scriptHappySession
// PURPOSE: Reuses one accepted session so both security modes share the same outcome.
void scriptHappySession(FakeChannel& channel, bool startTlsMode) {
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, startTlsMode);
  if (startTlsMode) {
    channel.enqueueReply("220 2.0.0 Ready to start TLS");
    enqueueEhlo(channel, true);
  }
  channel.enqueueReply("334 VXNlcm5hbWU6");  // "Username:"
  channel.enqueueReply("334 UGFzc3dvcmQ6");  // "Password:"
  channel.enqueueReply("235 2.7.0 Accepted");
  channel.enqueueReply("250 2.1.0 OK");
  channel.enqueueReply("250 2.1.5 OK");
  channel.enqueueReply("354 Go ahead");
  channel.enqueueReply("250 2.0.0 OK: queued");
  channel.enqueueReply("221 2.0.0 closing connection");

  if (startTlsMode) {
    channel.expectCommand("EHLO sms-gate.local");
    channel.expectCommand("STARTTLS");
  }
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("AUTH LOGIN");
  channel.expectCommand("dXNlckBleGFtcGxlLmNvbQ==");
  channel.expectCommand("c2VjcmV0LXBhc3N3b3JkLTEyMw==");
  channel.expectCommand("MAIL FROM:<device@example.com>");
  channel.expectCommand("RCPT TO:<owner@example.com>");
  channel.expectCommand("DATA");
  channel.expectCommand("QUIT");
}
// #endregion FUNC_scriptHappySession

// #region FUNC_testStartTlsHappyPath
// PURPOSE: Protects the complete STARTTLS submission path from dialog regressions.
void testStartTlsHappyPath() {
  FakeChannel channel;
  scriptHappySession(channel, true);

  SmtpConfigRecord record = makeRecord();
  SmtpClient client(channel);
  static int traceCount = 0;
  client.setStageListener([](const char* stage, int code) {
    assert(strlen(stage) > 0);
    (void)code;
    ++traceCount;
  });
  const SmtpSendResult result = client.sendMail(record, "sms-gate.local", "SMS from +79001234567",
                                                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
  assert(result == SmtpSendResult::kSuccess);
  assert(traceCount > 0);  // Banner, EHLO x2, AUTH x3, MAIL, RCPT, DATA, dot.
  assert(strcmp(client.failedStage(), "") == 0);
  assert(client.lastReplyCode() == 0);
  assert(channel.startTlsCalled);
  assert(!channel.connectImplicitTls);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());

  const char* written = channel.written();
  assert(strstr(written, "Subject: SMS from +79001234567\r\n") != nullptr);
  assert(strstr(written, "From: <device@example.com>\r\n") != nullptr);
  assert(strstr(written, "To: <owner@example.com>\r\n") != nullptr);
  assert(strstr(written, "Content-Transfer-Encoding: base64\r\n") != nullptr);
  assert(strstr(written, "\r\n0J/RgNC40LLQtdGC\r\n.\r\n") != nullptr);
  printf("testStartTlsHappyPath ok\n");
}
// #endregion FUNC_testStartTlsHappyPath

// #region FUNC_testImplicitTlsHappyPath
// PURPOSE: Protects implicit-TLS submission from accidental STARTTLS negotiation.
void testImplicitTlsHappyPath() {
  FakeChannel channel;
  scriptHappySession(channel, false);

  SmtpConfigRecord record = makeRecord();
  record.port = 465;
  record.securityMode = static_cast<uint8_t>(SmtpSecurityMode::kImplicitTls);
  record.checksum = calculateSmtpConfigChecksum(record);
  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(record, "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kSuccess);
  assert(channel.connectImplicitTls);
  assert(!channel.startTlsCalled);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  printf("testImplicitTlsHappyPath ok\n");
}
// #endregion FUNC_testImplicitTlsHappyPath

// #region FUNC_testStartTlsUnsupported
// PURPOSE: Prevents credentials from crossing a server connection without TLS.
void testStartTlsUnsupported() {
  FakeChannel channel;
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, false);
  channel.expectCommand("EHLO sms-gate.local");

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kTlsUnavailable);
  assert(!channel.startTlsCalled);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  assert(strstr(channel.written(), "AUTH LOGIN") == nullptr);
  printf("testStartTlsUnsupported ok\n");
}
// #endregion FUNC_testStartTlsUnsupported

// #region FUNC_testStartTlsRefused
// PURPOSE: Keeps authentication blocked when a server refuses STARTTLS.
void testStartTlsRefused() {
  FakeChannel channel;
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, true);
  channel.enqueueReply("454 4.7.0 TLS not available");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("STARTTLS");

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kTlsUnavailable);
  assert(!channel.startTlsCalled);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  printf("testStartTlsRefused ok\n");
}
// #endregion FUNC_testStartTlsRefused

// #region FUNC_testTlsUpgradeFailed
// PURPOSE: Prevents authentication after a failed TLS handshake.
void testTlsUpgradeFailed() {
  FakeChannel channel;
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, true);
  channel.enqueueReply("220 2.0.0 Ready to start TLS");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("STARTTLS");
  channel.failStartTls = true;

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kTlsFailed);
  assert(strcmp(client.failedStage(), "starttls_handshake") == 0);
  assert(client.lastReplyCode() == 0);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  printf("testTlsUpgradeFailed ok\n");
}
// #endregion FUNC_testTlsUpgradeFailed

// #region FUNC_testAuthRejected
// PURPOSE: Keeps rejected credentials distinguishable from transport failures.
void testAuthRejected() {
  FakeChannel channel;
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, true);
  channel.enqueueReply("220 2.0.0 Ready to start TLS");
  enqueueEhlo(channel, true);
  channel.enqueueReply("334 VXNlcm5hbWU6");
  channel.enqueueReply("334 UGFzc3dvcmQ6");
  channel.enqueueReply("535 5.7.8 Authentication failed");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("STARTTLS");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("AUTH LOGIN");
  channel.expectCommand("dXNlckBleGFtcGxlLmNvbQ==");
  channel.expectCommand("c2VjcmV0LXBhc3N3b3JkLTEyMw==");

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kAuthRejected);
  assert(strcmp(client.failedStage(), "auth_password") == 0);
  assert(client.lastReplyCode() == 535);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  printf("testAuthRejected ok\n");
}
// #endregion FUNC_testAuthRejected

// #region FUNC_testRecipientRejected
// PURPOSE: Keeps recipient rejection distinct after authentication succeeds.
void testRecipientRejected() {
  FakeChannel channel;
  channel.enqueueReply("220 smtp.example.com ESMTP ready");
  enqueueEhlo(channel, true);
  channel.enqueueReply("220 2.0.0 Ready to start TLS");
  enqueueEhlo(channel, true);
  channel.enqueueReply("334 VXNlcm5hbWU6");
  channel.enqueueReply("334 UGFzc3dvcmQ6");
  channel.enqueueReply("235 2.7.0 Accepted");
  channel.enqueueReply("250 2.1.0 OK");
  channel.enqueueReply("550 5.1.1 User unknown");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("STARTTLS");
  channel.expectCommand("EHLO sms-gate.local");
  channel.expectCommand("AUTH LOGIN");
  channel.expectCommand("dXNlckBleGFtcGxlLmNvbQ==");
  channel.expectCommand("c2VjcmV0LXBhc3N3b3JkLTEyMw==");
  channel.expectCommand("MAIL FROM:<device@example.com>");
  channel.expectCommand("RCPT TO:<owner@example.com>");

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kMessageRejected);
  assert(channel.stopCalledOnce());
  assert(channel.expectationsMet());
  printf("testRecipientRejected ok\n");
}
// #endregion FUNC_testRecipientRejected

// #region FUNC_testBareReplyCode
// PURPOSE: Accepts a three-digit SMTP reply without reading past its terminator.
void testBareReplyCode() {
  FakeChannel channel;
  channel.enqueueReply("500");

  SmtpClient client(channel);
  const SmtpSendResult result = client.sendMail(makeRecord(), "sms-gate.local", "Relay", "hi");
  assert(result == SmtpSendResult::kDialogFailed);
  assert(strcmp(client.failedStage(), "banner") == 0);
  assert(client.lastReplyCode() == 500);
  assert(channel.stopCalledOnce());
  assert(channel.written()[0] == '\0');
  printf("testBareReplyCode ok\n");
}
// #endregion FUNC_testBareReplyCode

}  // namespace

int main() {
  testRecordValidation();
  testStartTlsHappyPath();
  testImplicitTlsHappyPath();
  testStartTlsUnsupported();
  testStartTlsRefused();
  testTlsUpgradeFailed();
  testAuthRejected();
  testRecipientRejected();
  testBareReplyCode();
  printf("all smtp tests passed\n");
  return 0;
}
