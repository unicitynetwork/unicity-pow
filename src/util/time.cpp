// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "util/time.hpp"
#include <atomic>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace unicity {
namespace util {

// Static storage for mock time
// 0 means mock time is disabled (use real time)
static std::atomic<int64_t> g_mock_time{0};

// Mutex to protect steady clock state (fixes data race on g_real_steady_reference)
static std::mutex g_steady_mutex;

// Store the steady clock reference point when mock time is first set
// This allows us to simulate steady_clock behavior with mock time
// Protected by g_steady_mutex
static std::chrono::steady_clock::time_point g_real_steady_reference;
static int64_t g_mock_steady_reference{0};
static bool g_steady_initialized{false};

int64_t GetTime() {
  int64_t mock = g_mock_time.load(std::memory_order_relaxed);
  if (mock != 0) {
    return mock;
  }

  // Return real system time
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::chrono::steady_clock::time_point GetSteadyTime() {
  int64_t mock = g_mock_time.load(std::memory_order_relaxed);
  if (mock != 0) {
    // When mock time is active, simulate steady clock
    // Maintain the property that steady_clock advances monotonically

    std::lock_guard<std::mutex> lock(g_steady_mutex);

    if (!g_steady_initialized) {
      // First time using mock steady time - set reference point
      g_real_steady_reference = std::chrono::steady_clock::now();
      g_mock_steady_reference = mock;
      g_steady_initialized = true;
    }

    // Calculate offset from reference point
    int64_t seconds_offset = mock - g_mock_steady_reference;

    // Add offset to real reference point
    return g_real_steady_reference + std::chrono::seconds(seconds_offset);
  }

  // Return real steady clock time
  return std::chrono::steady_clock::now();
}

void SetMockTime(int64_t time) {
  g_mock_time.store(time, std::memory_order_relaxed);

  // Reset steady clock reference when mock time is changed
  // This ensures clean state for new mock time value
  if (time != 0) {
    std::lock_guard<std::mutex> lock(g_steady_mutex);
    g_steady_initialized = false;
    // g_real_steady_reference and g_mock_steady_reference will be
    // re-initialized on next GetSteadyTime() call
  }
}

int64_t GetMockTime() { return g_mock_time.load(std::memory_order_relaxed); }

std::string FormatTime(int64_t unix_time) {
  if (unix_time == 0) {
    return "1970-01-01 00:00:00 UTC";
  }

  std::time_t t = static_cast<std::time_t>(unix_time);

  // Use thread-safe gmtime_r (POSIX) or gmtime_s (Windows)
  std::tm tm_utc;

#if defined(_WIN32)
  // Windows: gmtime_s has different signature (dest, src)
  if (gmtime_s(&tm_utc, &t) != 0) {
    return "invalid";
  }
#else
  // POSIX: gmtime_r has signature (src, dest)
  if (!gmtime_r(&t, &tm_utc)) {
    return "invalid";
  }
#endif

  std::ostringstream oss;
  oss << std::put_time(&tm_utc, "%Y-%m-%d %H:%M:%S UTC");
  return oss.str();
}

} // namespace util
} // namespace unicity
