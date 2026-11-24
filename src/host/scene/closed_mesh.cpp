// SPDX-License-Identifier: Apache-2.0
#include "host/scene/closed_mesh.h"

#include <algorithm>
#include <vector>

namespace astroray::host {

bool triangles_closed(const uint32_t* tri_vertex, size_t triangle_count) {
  if (triangle_count == 0) return false;
  // Directed edges a -> b. A closed oriented surface uses each one exactly once and also its reverse
  // exactly once, so sorted, the forward and the reversed lists are equal and free of duplicates.
  std::vector<uint64_t> forward, reverse;
  forward.reserve(triangle_count * 3);
  reverse.reserve(triangle_count * 3);
  for (size_t t = 0; t < triangle_count; ++t) {
    for (int k = 0; k < 3; ++k) {
      const uint64_t a = tri_vertex[3 * t + k], b = tri_vertex[3 * t + (k + 1) % 3];
      if (a == b) return false;
      forward.push_back(a << 32 | b);
      reverse.push_back(b << 32 | a);
    }
  }
  std::sort(forward.begin(), forward.end());
  std::sort(reverse.begin(), reverse.end());
  if (std::adjacent_find(forward.begin(), forward.end()) != forward.end()) return false;
  return forward == reverse;
}

}  // namespace astroray::host
