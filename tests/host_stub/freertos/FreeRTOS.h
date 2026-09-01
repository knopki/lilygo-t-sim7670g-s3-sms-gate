#pragma once

#include <atomic>
#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;

constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdTRUE = 1;

struct portMUX_TYPE {
  std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define pdMS_TO_TICKS(milliseconds) (milliseconds)

inline void portENTER_CRITICAL(portMUX_TYPE* mux) {
  while (mux->locked.test_and_set(std::memory_order_acquire)) {
  }
}

inline void portEXIT_CRITICAL(portMUX_TYPE* mux) { mux->locked.clear(std::memory_order_release); }
