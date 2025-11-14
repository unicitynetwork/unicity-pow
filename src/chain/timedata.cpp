// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "chain/timedata.hpp"
#include "network/protocol.hpp"
#include "util/logging.hpp"
#include <ctime>
#include <mutex>
#include <set>

namespace unicity {
namespace chain {

// Global state (thread-safe via mutex)
static std::mutex g_timeoffset_mutex;
static int64_t nTimeOffset = 0; // Current network time offset in seconds

#define BITCOIN_TIMEDATA_MAX_SAMPLES 200

static std::set<protocol::NetworkAddress> g_sources;
static CMedianFilter<int64_t> g_time_offsets{BITCOIN_TIMEDATA_MAX_SAMPLES, 0};
static bool g_warning_emitted = false;

/**
 * "Never go to sea with two chronometers; take one or three."
 * Our three time sources are:
 *  - System clock
 *  - Median of other nodes clocks
 *  - The user (asking the user to fix the system clock if the first two
 * disagree)
 */
int64_t GetTimeOffset() {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);
  return nTimeOffset;
}

void AddTimeData(const protocol::NetworkAddress& ip, int64_t nOffsetSample) {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);

  // Ignore duplicates
  if (g_sources.size() == BITCOIN_TIMEDATA_MAX_SAMPLES)
    return;
  if (!g_sources.insert(ip).second)
    return;

  // Add data
  g_time_offsets.input(nOffsetSample);
  LOG_CHAIN_TRACE("added time data, samples {}, offset {:+d} ({:+d} minutes)",
                  g_time_offsets.size(), nOffsetSample, nOffsetSample / 60);

  // There is a known issue here (see issue #4521):
  //
  // - The structure g_time_offsets contains up to 200 elements, after which
  // any new element added to it will not increase its size, replacing the
  // oldest element.
  //
  // - The condition to update nTimeOffset includes checking whether the
  // number of elements in g_time_offsets is odd, which will never happen after
  // there are 200 elements.
  //
  // But in this case the 'bug' is protective against some attacks, and may
  // actually explain why we've never seen attacks which manipulate the
  // clock offset.
  //
  // So we should hold off on fixing this and clean it up as part of
  // a timing cleanup that strengthens it in a number of other ways.
  //
  if (g_time_offsets.size() >= 5 && g_time_offsets.size() % 2 == 1) {
    int64_t nMedian = g_time_offsets.median();
    std::vector<int64_t> vSorted = g_time_offsets.sorted();
    // Only let other nodes change our time by so much
    int64_t max_adjustment = DEFAULT_MAX_TIME_ADJUSTMENT;
    if (nMedian >= -max_adjustment && nMedian <= max_adjustment) {
      nTimeOffset = nMedian;
    } else {
      nTimeOffset = 0;

      if (!g_warning_emitted) {
        // If nobody has a time different than ours but within 5 minutes of ours, give a warning
        bool fMatch = false;
        for (const int64_t nOffset : vSorted) {
          if (nOffset != 0 && nOffset > -5 * 60 && nOffset < 5 * 60) fMatch = true;
        }

        if (!fMatch) {
          g_warning_emitted = true;
          LOG_CHAIN_ERROR("Please check that your computer's date and time are correct! If your clock is wrong, Unicity will not work properly.");
        }
      }
    }

    // Debug logging of all time samples
    std::string log_message{"time data samples: "};
    for (const int64_t n : vSorted) {
      log_message += std::to_string(n) + "  ";
    }
    log_message += "|  median offset = " + std::to_string(nTimeOffset) + "  (" + std::to_string(nTimeOffset / 60) + " minutes)";
    LOG_CHAIN_TRACE("{}", log_message);
  }
}

void TestOnlyResetTimeData() {
  std::lock_guard<std::mutex> lock(g_timeoffset_mutex);
  nTimeOffset = 0;
  g_sources.clear();
  g_time_offsets = CMedianFilter<int64_t>{BITCOIN_TIMEDATA_MAX_SAMPLES, 0};
  g_warning_emitted = false;
}

} // namespace chain
} // namespace unicity
