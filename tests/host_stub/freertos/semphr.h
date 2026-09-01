#pragma once

#include "freertos/FreeRTOS.h"

struct StubSemaphore {};
using SemaphoreHandle_t = StubSemaphore*;

namespace freertos_test {

inline bool takeSucceeds = true;
inline unsigned takeCalls = 0;
inline unsigned giveCalls = 0;
inline TickType_t lastTimeout = 0;

inline void reset(bool nextTakeSucceeds) {
  takeSucceeds = nextTakeSucceeds;
  takeCalls = 0;
  giveCalls = 0;
  lastTimeout = 0;
}

}  // namespace freertos_test

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  static StubSemaphore mutex;
  return &mutex;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t timeout) {
  ++freertos_test::takeCalls;
  freertos_test::lastTimeout = timeout;
  return freertos_test::takeSucceeds ? pdTRUE : pdFALSE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) {
  ++freertos_test::giveCalls;
  return pdTRUE;
}
