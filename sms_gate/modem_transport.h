// #region MODULE_CONTRACT
// PURPOSE: Binds HardwareSerial Serial1 to ModemChannel for the Classic
// LilyGO T-SIM7670G-S3 pin map (ADR-0004). Owns UART, PWRKEY/DTR/RESET
// sequencing and timeouts; parsing stays in modem_client.*.
// SCOPE:
// - Serial1 begin/end (115200 8N1, RX=GPIO10 TX=GPIO11), DTR LOW=awake,
//   PWRKEY pulse, RESET handling, ModemChannel::write/readLine/purge.
// - NOT: AT command sequencing or response parsing (modem_client),
//   SMTP/HTTP/NVS.
// INVARIANTS: DTR held LOW while active; every readLine respects timeoutMs
// and never overruns buffer; purge discards stale URCs before next command.
// DEPENDENCIES: Arduino HardwareSerial, esp32-hal-gpio.
// #endregion MODULE_CONTRACT

#pragma once

#include <Arduino.h>

#include "modem_client.h"

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

  bool begin() {
    if (started_) return true;
    pinMode(kModemPinDtr, OUTPUT);
    pinMode(kModemPinReset, OUTPUT);
    pinMode(kModemPinPwrKey, OUTPUT);
    digitalWrite(kModemPinDtr, LOW);
    digitalWrite(kModemPinReset, HIGH);
    digitalWrite(kModemPinPwrKey, LOW);
    Serial1.begin(kModemBaud, SERIAL_8N1, kModemPinRx, kModemPinTx);
    started_ = true;
    return true;
  }

  void end() {
    if (!started_) return;
    Serial1.end();
    started_ = false;
  }

  // Power-on pulse per LilyGO: PWRKEY LOW 100ms → HIGH 100ms → LOW.
  void powerPulse() {
    digitalWrite(kModemPinPwrKey, LOW);
    delay(100);
    digitalWrite(kModemPinPwrKey, HIGH);
    delay(100);
    digitalWrite(kModemPinPwrKey, LOW);
  }

  // ModemChannel overrides.
  bool write(const char* data, size_t len) override {
    if (!started_) return false;
    return Serial1.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  }

  int readLine(char* buffer, size_t size, unsigned long timeoutMs) override {
    if (size < 2) return -1;
    const unsigned long deadline = millis() + timeoutMs;
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
      if (millis() > deadline) {
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

  void purge() override {
    if (!started_) return;
    while (Serial1.available() > 0) Serial1.read();
  }

 private:
  bool started_ = false;
};
// #endregion CLASS_ModemTransport
