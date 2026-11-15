// SPDX-License-Identifier: Apache-2.0
#include "regalloc.h"

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace astroray::host {

bool allocate_registers(const std::vector<LiveRange>& ranges, std::vector<int>* reg, std::string* err) {
  const int n = int(ranges.size());
  reg->assign(size_t(n), -1);
  std::vector<int> order(static_cast<size_t>(ranges.size()));
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ranges[size_t(a)].start < ranges[size_t(b)].start; });

  // Peak demand: the most registers live at once, reported when the allocation fails.
  int peak = 0;
  for (int i = 0; i < n; ++i) {
    int live = 0;
    for (int j = 0; j < n; ++j)
      if (ranges[size_t(j)].start <= ranges[size_t(i)].start && ranges[size_t(j)].end >= ranges[size_t(i)].start)
        live += ranges[size_t(j)].width;
    peak = std::max(peak, live);
  }

  uint32_t used = 0;           // bit r set while register r holds a live value
  std::vector<int> active;     // indices, kept sorted by increasing end (the paper's active list)
  for (int i : order) {
    const LiveRange& r = ranges[size_t(i)];
    while (!active.empty() && ranges[size_t(active.front())].end < r.start) {
      const int e = active.front();
      active.erase(active.begin());
      used &= ~(((1u << ranges[size_t(e)].width) - 1u) << (*reg)[size_t(e)]);
    }
    const uint32_t mask = (1u << r.width) - 1u;
    int found = -1;
    for (int p = 0; p + r.width <= kRegisterCount; ++p)
      if ((used & (mask << p)) == 0u) {
        found = p;
        break;
      }
    if (found < 0) {
      if (peak > kRegisterCount)
        *err = "needs " + std::to_string(peak) + " registers, " + std::to_string(kRegisterCount) + " available";
      else
        *err = "could not place " + std::to_string(r.width) + " wide value; peak " + std::to_string(peak) + " of " +
               std::to_string(kRegisterCount);
      return false;
    }
    (*reg)[size_t(i)] = found;
    used |= mask << found;
    auto pos = std::upper_bound(active.begin(), active.end(), i,
                                [&](int a, int b) { return ranges[size_t(a)].end < ranges[size_t(b)].end; });
    active.insert(pos, i);
  }
  return true;
}

}  // namespace astroray::host
