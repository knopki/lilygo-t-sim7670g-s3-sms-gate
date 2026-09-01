// #region MODULE_CONTRACT
// PURPOSE: Isolates ZTE LAN transport from testable goform protocol logic.
// SCOPE:
// - Bounded TCP connect, HTTP line/body reads, and teardown.
// - NOT: goform protocol, JSON scanning, persistence, or HTTP routes.
// INVARIANTS:
// - Reads avoid the unreliable select path;
// - transient socket errors wait until the deadline and all buffers remain bounded;
// - an optional progress hook runs around blocking I/O.
// DEPENDENCIES: NetworkClient and lwIP sockets.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_TRANSPORT_H
#define ZTE_ZTE_TRANSPORT_H

#include <errno.h>

#include <Arduino.h>
#include <NetworkClient.h>
#include <lwip/sockets.h>

#include "system/millis_deadline.h"
#include "system/plain_socket_reader.h"
#include "zte/zte_client.h"

// #region CLASS_NetworkZteChannel
// PURPOSE: One ZteChannel implementation per dialog instance so the socket
// and its buffers are released together with the object.
class NetworkZteChannel : public ZteChannel {
 public:
  // keepAlive runs in the caller's task around each bounded network operation.
  // The poll task supplies watchdog::reset; tests and one-shot UI tasks omit it.
  explicit NetworkZteChannel(void (*keepAlive)() = nullptr, unsigned long timeoutMs = 10000)
      : keepAlive_(keepAlive), timeoutMs_(timeoutMs) {}

  bool connect(const char* host, uint16_t port) override {
    recvTimeoutSet_ = false;
    reportProgress();
    const bool connected = client_.connect(host, port, timeoutMs_) == 1;
    reportProgress();
    return connected;
  }

  bool write(const char* data, size_t length) override {
    const uint32_t deadline = millis() + timeoutMs_;
    size_t sent = 0;
    while (sent < length) {
      reportProgress();
      const size_t written =
          client_.write(reinterpret_cast<const uint8_t*>(data + sent), length - sent);
      if (written > 0) {
        sent += written;
        continue;
      }
      if (millis_deadline::reached(millis(), deadline)) {
        return false;
      }
      delay(1);
    }
    reportProgress();
    return true;
  }

  int readLine(char* buffer, size_t size) override {
    if (size < 2) {
      return -1;
    }
    reportProgress();
    const uint32_t deadline = millis() + timeoutMs_;
    ensureRecvTimeout();
    const int result = plainReadLine(client_.fd(), buffer, size, deadline);
    reportProgress();
    return result;
  }

  int read(char* buffer, size_t size) override {
    reportProgress();
    ensureRecvTimeout();
    const uint32_t deadline = millis() + timeoutMs_;
    const int result = plainReadBytes(client_.fd(), buffer, size, deadline);
    reportProgress();
    return result;
  }

  void stop() override { client_.stop(); }

 private:
  void reportProgress() const {
    if (keepAlive_ != nullptr) {
      keepAlive_();
    }
  }

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
  void (*keepAlive_)();
  unsigned long timeoutMs_;
  bool recvTimeoutSet_ = false;
};
// #endregion CLASS_NetworkZteChannel
#endif  // ZTE_ZTE_TRANSPORT_H
