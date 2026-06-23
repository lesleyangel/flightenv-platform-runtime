#include "FlightEnvPlatformRuntime/RuntimeClock.hpp"

#include <chrono>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#endif

namespace FlightEnvPlatformRuntime {

std::string RuntimeClock::nowUtcIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t time = clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

std::int64_t RuntimeClock::steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t RuntimeClock::secondsToNanoseconds(double seconds) {
  return static_cast<std::int64_t>(seconds * 1000000000.0);
}

double RuntimeClock::nanosecondsToSeconds(std::int64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / 1000000000.0;
}

}  // namespace FlightEnvPlatformRuntime
