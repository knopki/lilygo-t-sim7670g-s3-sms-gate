// #region MODULE_CONTRACT
// PURPOSE: Binds the host-testable SMTP dialog to the bundled
// NetworkClientSecure transport with standard Mozilla root validation.
// SCOPE:
// - TCP connect (plain or implicit TLS), line reads with deadlines, the
// STARTTLS upgrade point, and clean teardown.
// - NOT: SMTP sequencing, persistence, and HTML rendering.
// INVARIANTS: The plain phase never touches the core's lwIP select path
// (available() there does a zero-timeout select that returns spurious
// -1/EINTR under concurrent selects on core 3.x, confirmed on hardware);
// plain reads use one bounded recv() per byte instead. TLS-phase reads keep
// the core's mbedTLS path proven by implicit-TLS deliveries. connected() is
// never called in the plain phase. Every connection validates the server
// chain, expiry, and hostname against the embedded root bundle; there is no
// insecure mode.
// DEPENDENCIES: Uses Arduino-ESP32 NetworkClientSecure and the Mozilla CA
// bundle linked into the core (esp_crt_bundle).
// #endregion MODULE_CONTRACT

#pragma once

#include <errno.h>

#include <Arduino.h>
#include <NetworkClientSecure.h>
#include <lwip/sockets.h>

#include "plain_socket_reader.h"
#include "smtp_client.h"

extern "C" {
// Mozilla CA bundle embedded in the firmware image by the core's prebuilt
// mbedTLS (see ADR-0002 for the trust decision).
extern const uint8_t _binary_x509_crt_bundle_start[];
extern const uint8_t _binary_x509_crt_bundle_end[];
}

// #region CLASS_SecureSmtpChannel
// PURPOSE: One SmtpChannel implementation per submission attempt so the TLS
// client and its heap are released together with the object.
class SecureSmtpChannel : public SmtpChannel {
 public:
  explicit SecureSmtpChannel(unsigned long timeoutMs = 15000) : timeoutMs_(timeoutMs) {
    const size_t bundleSize = reinterpret_cast<size_t>(_binary_x509_crt_bundle_end) -
                              reinterpret_cast<size_t>(_binary_x509_crt_bundle_start);
    client_.setCACertBundle(_binary_x509_crt_bundle_start, bundleSize);
    client_.setHandshakeTimeout(timeoutMs / 1000);
  }

  // Why the last readLine failed: 'T' deadline, 'S' available() error (the
  // core closes the socket in that branch), 'R' read() error or EOF,
  // 'F' peer closed (plain recv), 'L' line overflow.
  char readDetail() const { return readDetail_; }
  // errno captured at that failure (0 when none); lwIP sets it on recv.
  int lastErrno() const { return lastErrno_; }

  bool connect(const char* host, uint16_t port, bool implicitTls) override {
    if (!implicitTls) {
      // Postpone the TLS handshake until STARTTLS has been accepted.
      client_.setPlainStart();
    }
    return client_.connect(host, port, timeoutMs_) == 1;
  }

  bool write(const char* data, size_t length) override {
    const unsigned long deadline = millis() + timeoutMs_;
    size_t sent = 0;
    while (sent < length) {
      const size_t written =
          client_.write(reinterpret_cast<const uint8_t*>(data + sent), length - sent);
      if (written > 0) {
        sent += written;
        continue;
      }
      if (millis() > deadline) {
        return false;
      }
      delay(1);
    }
    return true;
  }

  int readLine(char* buffer, size_t size) override {
    if (size < 2) {
      return -1;
    }
    const unsigned long deadline = millis() + timeoutMs_;
    size_t used = 0;
    if (client_.stillInPlainStart()) {
      return readPlainLine(buffer, size, deadline);
    }
    for (;;) {
      const int available = client_.available();
      if (available < 0) {
        readDetail_ = 'S';
        lastErrno_ = errno;
        return -1;
      }
      if (available > 0) {
        const int character = client_.read();
        if (character < 0) {
          readDetail_ = 'R';
          lastErrno_ = errno;
          return -1;
        }
        if (character == '\n') {
          if (used > 0 && buffer[used - 1] == '\r') {
            --used;
          }
          buffer[used] = '\0';
          return static_cast<int>(used);
        }
        if (used + 1 >= size) {
          readDetail_ = 'L';
          lastErrno_ = 0;
          return -1;  // Line does not fit; treated as a protocol failure.
        }
        buffer[used++] = static_cast<char>(character);
        continue;
      }
      if (millis() > deadline) {
        readDetail_ = 'T';
        lastErrno_ = 0;
        return -1;
      }
      delay(1);
    }
  }

  bool startTls() override { return client_.startTLS() == 1; }

  void stop() override { client_.stop(); }

 private:
  // Plain-phase line reader delegates to plain_socket_reader.h (ADR-0002).
  int readPlainLine(char* buffer, size_t size, unsigned long deadline) {
    if (!recvTimeoutSet_) {
      struct timeval tv = {};
      tv.tv_sec = static_cast<long>(timeoutMs_ / 1000);
      tv.tv_usec = static_cast<long>((timeoutMs_ % 1000) * 1000);
      if (client_.setSocketOption(SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv)) >= 0) {
        recvTimeoutSet_ = true;
      }
    }
    return plainReadLineDetailed(client_.fd(), buffer, size, deadline, readDetail_, lastErrno_);
  }

  NetworkClientSecure client_;
  unsigned long timeoutMs_;
  bool recvTimeoutSet_ = false;
  char readDetail_ = 0;
  int lastErrno_ = 0;
};
// #endregion CLASS_SecureSmtpChannel
