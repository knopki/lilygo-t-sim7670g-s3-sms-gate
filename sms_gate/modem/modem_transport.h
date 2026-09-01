// #region MODULE_CONTRACT
// PURPOSE: Isolates SIM7670G pin sequencing from testable AT protocol logic.
// SCOPE:
// - Serial1 begin/end, DTR LOW=awake, PWRKEY pulse,
// - RESET handling, ModemChannel::write/readLine/purge.
// - NOT: AT command sequencing or response parsing (modem_client), SMTP/HTTP/NVS.
// INVARIANTS:
// - DTR held LOW while active;
// - every readLine respects timeoutMs and never overruns buffer;
// - purge discards stale URCs before next command.
// DEPENDENCIES: Arduino HardwareSerial, esp32-hal-gpio.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_MODEM_TRANSPORT_H
#define MODEM_MODEM_TRANSPORT_H

#include <Arduino.h>

#include "modem/modem_client.h"
#include "system/millis_deadline.h"

// Classic revision pin map (see docs/research/modem-sim7670g.md §1).
constexpr int kModemPinRx = 10;
constexpr int kModemPinTx = 11;
constexpr int kModemPinPwrKey = 18;
constexpr int kModemPinDtr = 9;
constexpr int kModemPinReset = 17;
constexpr uint32_t kModemBaud = 115200;

// #region CLASS_ModemTransport
// PURPOSE: HardwareSerial implementation of ModemChannel for the device.
// Owns Serial1 lifecycle and the Classic power-on sequence.
class ModemTransport : public ModemChannel {
 public:
  ModemTransport() = default;

  // #region METHOD_ModemTransport_begin
  // PURPOSE: Starts the shared modem UART and establishes safe pin states.
  bool begin() {
    if (started_) return true;
    // Pins must be configured even when Serial1 already active (shared bus).
    pinMode(kModemPinDtr, OUTPUT);
    pinMode(kModemPinReset, OUTPUT);
    pinMode(kModemPinPwrKey, OUTPUT);
    digitalWrite(kModemPinDtr, LOW);
    digitalWrite(kModemPinReset, HIGH);
    digitalWrite(kModemPinPwrKey, LOW);
    if (!s_serialActive) {
      Serial1.begin(kModemBaud, SERIAL_8N1, kModemPinRx, kModemPinTx);
      s_serialActive = true;
    }
    s_refCount++;
    started_ = true;
    return true;
  }

  // #endregion METHOD_ModemTransport_begin

  // #region METHOD_ModemTransport_end
  // PURPOSE: Releases this transport's UART reference without disrupting peers.
  void end() {
    if (!started_) return;
    started_ = false;
    if (s_refCount > 0) s_refCount--;
    if (s_refCount == 0 && s_serialActive) {
      Serial1.end();
      s_serialActive = false;
    }
  }

  // Power-on pulse per LilyGO: PWRKEY LOW 100ms → HIGH 100ms → LOW.
  // #endregion METHOD_ModemTransport_end

  // #region METHOD_ModemTransport_powerPulse
  // PURPOSE: Performs the board-specific pulse that wakes the modem.
  void powerPulse() {
    digitalWrite(kModemPinPwrKey, LOW);
    delay(100);
    digitalWrite(kModemPinPwrKey, HIGH);
    delay(100);
    digitalWrite(kModemPinPwrKey, LOW);
  }

  // ModemChannel overrides.
  // #endregion METHOD_ModemTransport_powerPulse

  // #region METHOD_ModemTransport_write
  // PURPOSE: Writes one complete AT command only while the UART is active.
  bool write(const char* data, size_t len) override {
    if (!started_) return false;
    return Serial1.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  }

  // #endregion METHOD_ModemTransport_write

  // #region METHOD_ModemTransport_readLine
  // PURPOSE: Reads one bounded modem line while preserving timeout semantics.
  int readLine(char* buffer, size_t size, unsigned long timeoutMs) override {
    if (size < 2) return -1;
    const uint32_t deadline = millis() + timeoutMs;
    size_t used = 0;
    bool overflow = false;
    for (;;) {
      if (Serial1.available() > 0) {
        int ch = Serial1.read();
        if (ch < 0) continue;
        if (ch == '\n') {
          if (overflow) {
            buffer[0] = '\0';
            return -1;
          }
          if (used > 0 && buffer[used - 1] == '\r') --used;
          buffer[used] = '\0';
          return static_cast<int>(used);
        }
        if (overflow) continue;
        if (used + 1 >= size) {
          overflow = true;
          continue;
        }
        buffer[used++] = static_cast<char>(ch);
        continue;
      }
      if (millis_deadline::reached(millis(), deadline)) {
        if (overflow) {
          buffer[0] = '\0';
          return -1;
        }
        if (used < size) {
          buffer[used] = '\0';
        } else if (size > 0) {
          buffer[size - 1] = '\0';
        }
        if (used > 0 && buffer[used - 1] == '\r') {
          buffer[--used] = '\0';
        }
        return used == 0 ? -1 : static_cast<int>(used);
      }
      delay(1);
    }
  }

  // #endregion METHOD_ModemTransport_readLine

  // #region METHOD_ModemTransport_purge
  // PURPOSE: Removes stale UART input before the next AT transaction.
  void purge() override {
    if (!started_) return;
    while (Serial1.available() > 0) Serial1.read();
  }
  // #endregion METHOD_ModemTransport_purge

 private:
  bool started_ = false;
  inline static int s_refCount = 0;
  inline static bool s_serialActive = false;
};
// #endregion CLASS_ModemTransport
#endif  // MODEM_MODEM_TRANSPORT_H
