#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

namespace EloqKV
{
// Randomly select `count `or `size` numbers from range [0-size). If repeat is
// true, numbers can be repeated, and outputs `count` numbers. If repeat is
// false, numbers are distinct, and outpus min(count, size) numbers.
//
// map.first is the selected random number. map.second is the index where the
// random number should be placed, some commands hope that the order of output
// is random, too.
void GenRandMap(int32_t size,
                int64_t count,
                bool repeat,
                std::multimap<int32_t, int64_t> &map);
}  // namespace EloqKV
