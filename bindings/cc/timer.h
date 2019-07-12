// timer.h - support for timers

#pragma once

extern "C" {
#include <base/time.h>
#include <runtime/timer.h>
}

namespace rt {

constexpr uint64_t kMilliseconds = 1000;
constexpr uint64_t kSeconds = 1000000;

// Gets the current number of microseconds since the launch of the runtime.
inline uint64_t MicroTime() {
  return microtime();
}

// Busy-spins for a microsecond duration.
inline void Delay(uint64_t us) {
  delay_us(us);
}

// Sleeps until a microsecond deadline.
inline void SleepUntil(uint64_t deadline_us) {
  timer_sleep_until(deadline_us);
}

// Sleeps for a microsecond duration.
inline void Sleep(uint64_t duration_us) {
  timer_sleep(duration_us);
}

} // namespace rt
