// #region MODULE_CONTRACT
// PURPOSE: Binds the host-testable ZTE goform dialog to the bundled
// NetworkClient transport on the modem's LAN segment (plain HTTP; see
// ADR-0003 for the trust discussion).
// SCOPE:
// - TCP connect with deadline, line reads for HTTP headers, exact and
// best-effort body reads, and clean teardown.
// - NOT: The goform protocol, JSON scanning, persistence, and HTTP route
// handling.
// INVARIANTS: Plain reads never touch the core's lwIP select path
// (available() there does a zero-timeout select that returns spurious
// -1/EINTR under concurrent selects on core 3.x, confirmed on hardware in
// ADR-0002); reads use bounded recv() instead. EAGAIN/EWOULDBLOCK/EINTR
// mean "no data yet" and the loop waits until the deadline.
// DEPENDENCIES: Uses Arduino-ESP32 NetworkClient and lwIP sockets.
// #endregion MODULE_CONTRACT

#pragma once

#include <errno.h>

#include <Arduino.h>
#include <NetworkClient.h>
#include <lwip/sockets.h>

#include "zte_client.h"

// #region CLASS_NetworkZteChannel
// PURPOSE: One ZteChannel implementation per dialog instance so the socket
// and its buffers are released together with the object.
class NetworkZteChannel : public ZteChannel {
 public:
  explicit NetworkZteChannel(unsigned long timeoutMs = 10000) : timeoutMs_(timeoutMs) {}

  bool connect(const char* host, uint16_t port) override {
    recvTimeoutSet_ = false;
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
    ensureRecvTimeout();
    size_t used = 0;
    for (;;) {
      uint8_t byte = 0;
      const int got = ::recv(client_.fd(), &byte, 1, 0);
      if (got == 0) {
        return -1;  // Peer closed the connection.
      }
      if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          if (millis() > deadline) {
            return -1;
          }
          delay(1);
          continue;
        }
        return -1;
      }
      if (byte == '\n') {
        if (used > 0 && buffer[used - 1] == '\r') {
          --used;
        }
        buffer[used] = '\0';
        return static_cast<int>(used);
      }
      if (used + 1 >= size) {
        return -1;  // Line does not fit; treated as a protocol failure.
      }
      buffer[used++] = static_cast<char>(byte);
      if (millis() > deadline) {
        return -1;
      }
    }
  }

  int read(char* buffer, size_t size) override {
    ensureRecvTimeout();
    const unsigned long deadline = millis() + timeoutMs_;
    size_t used = 0;
    while (used < size) {
      const int got = ::recv(client_.fd(), buffer + used, size - used, 0);
      if (got == 0) {
        break;  // Clean end of stream: return what was read so far.
      }
      if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          if (millis() > deadline) {
            break;
          }
          delay(1);
          continue;
        }
        break;
      }
      used += static_cast<size_t>(got);
    }
    return used > 0 ? static_cast<int>(used) : -1;
  }

  void stop() override { client_.stop(); }

 private:
  // One bounded recv() per byte/chunk with SO_RCVTIMEO; see INVARIANTS.
  void ensureRecvTimeout() {
    if (recvTimeoutSet_) {
      return;
    }
    struct timeval tv = {};
    tv.tv_sec = static_cast<long>(timeoutMs_ / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs_ % 1000) * 1000);
    if (client_.setSocketOption(SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv)) >= 0) {
      recvTimeoutSet_ = true;
    }
  }

  NetworkClient client_;
  unsigned long timeoutMs_;
  bool recvTimeoutSet_ = false;
};
// #endregion CLASS_NetworkZteChannel
