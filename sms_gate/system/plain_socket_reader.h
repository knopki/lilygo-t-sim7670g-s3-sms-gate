// #region MODULE_CONTRACT
// PURPOSE: Keeps plain-socket reads bounded and independent of flaky select().
// SCOPE:
// - Line reads and exact byte reads over an already-connected fd with
// SO_RCVTIMEO.
// - NOT: TLS reads, connect/write, and HTTP framing.
// INVARIANTS:
// - One recv() per byte/chunk, bounded by deadline via millis()+ delay(1);
// - never calls available() or select().
// DEPENDENCIES: Arduino millis/delay and lwIP sockets.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef SYSTEM_PLAIN_SOCKET_READER_H
#define SYSTEM_PLAIN_SOCKET_READER_H

#include <errno.h>

#include <Arduino.h>
#include <lwip/sockets.h>

// #region FUNC_plainReadLine
// PURPOSE: Reads one CRLF-terminated line (without CRLF) into buffer with
// SO_RCVTIMEO already set on fd; treats EAGAIN/EWOULDBLOCK/EINTR as wait
// until deadline.
inline int plainReadLine(int fd, char* buffer, size_t size, unsigned long deadline) {
  if (size < 2) {
    return -1;
  }
  size_t used = 0;
  for (;;) {
    uint8_t byte = 0;
    const int got = ::recv(fd, &byte, 1, 0);
    if (got == 0) {
      return -1;
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
      return -1;
    }
    buffer[used++] = static_cast<char>(byte);
    if (millis() > deadline) {
      return -1;
    }
  }
}
// #endregion FUNC_plainReadLine

// #region FUNC_plainReadLineDetailed
// PURPOSE: Same as plainReadLine but reports why it failed for SMTP
// diagnostics: 'T' deadline, 'F' peer closed, 'L' overflow, 'R' other.
inline int plainReadLineDetailed(int fd, char* buffer, size_t size, unsigned long deadline,
                                 char& detail, int& lastErrno) {
  detail = 0;
  lastErrno = 0;
  if (size < 2) {
    detail = 'L';
    return -1;
  }
  size_t used = 0;
  for (;;) {
    uint8_t byte = 0;
    const int got = ::recv(fd, &byte, 1, 0);
    if (got == 0) {
      detail = 'F';
      return -1;
    }
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        if (millis() > deadline) {
          detail = 'T';
          lastErrno = errno;
          return -1;
        }
        delay(1);
        continue;
      }
      detail = 'R';
      lastErrno = errno;
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
      detail = 'L';
      return -1;
    }
    buffer[used++] = static_cast<char>(byte);
    if (millis() > deadline) {
      detail = 'T';
      return -1;
    }
  }
}
// #endregion FUNC_plainReadLineDetailed

// #region FUNC_plainReadBytes
// PURPOSE: Reads up to size bytes (exact when peer keeps sending) with the
// same EAGAIN wait-until-deadline contract; returns bytes read or -1 when
// none arrived before deadline.
inline int plainReadBytes(int fd, char* buffer, size_t size, unsigned long deadline) {
  size_t used = 0;
  while (used < size) {
    const int got = ::recv(fd, buffer + used, size - used, 0);
    if (got == 0) {
      break;
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
// #endregion FUNC_plainReadBytes
#endif  // SYSTEM_PLAIN_SOCKET_READER_H
