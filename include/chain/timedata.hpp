// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace unicity {

namespace protocol {
struct NetworkAddress;
}

namespace chain {

//
// Maximum time adjustment from network peers (±70 minutes)
static constexpr int64_t DEFAULT_MAX_TIME_ADJUSTMENT = 70 * 60;

/**
 * Median filter over a stream of values.
 * Returns the median of the last N numbers
 *
 * NOTE: This implementation matches Bitcoin Core exactly, including known quirks:
 * - Starts with an initial value (typically 0) which affects early medians
 * - Potential integer overflow in median calculation for extreme values
 * - O(n log n) sorting on every input
 */
template <typename T>
class CMedianFilter
{
private:
    std::vector<T> vValues;
    std::vector<T> vSorted;
    unsigned int nSize;

public:
    CMedianFilter(unsigned int _size, T initial_value) : nSize(_size)
    {
        vValues.reserve(_size);
        vValues.push_back(initial_value);
        vSorted = vValues;
    }

    void input(T value)
    {
        if (vValues.size() == nSize) {
            vValues.erase(vValues.begin());
        }
        vValues.push_back(value);

        vSorted.resize(vValues.size());
        std::copy(vValues.begin(), vValues.end(), vSorted.begin());
        std::sort(vSorted.begin(), vSorted.end());
    }

    T median() const
    {
        int vSortedSize = vSorted.size();
        assert(vSortedSize > 0);
        if (vSortedSize & 1) // Odd number of elements
        {
            return vSorted[vSortedSize / 2];
        } else // Even number of elements
        {
            return (vSorted[vSortedSize / 2 - 1] + vSorted[vSortedSize / 2]) / 2;
        }
    }

    int size() const
    {
        return vValues.size();
    }

    std::vector<T> sorted() const
    {
        return vSorted;
    }
};

/** Functions to keep track of adjusted P2P time */
int64_t GetTimeOffset();
void AddTimeData(const protocol::NetworkAddress& ip, int64_t nOffsetSample);

/**
 * Reset the internal state of GetTimeOffset() and AddTimeData().
 */
void TestOnlyResetTimeData();

} // namespace chain
} // namespace unicity
