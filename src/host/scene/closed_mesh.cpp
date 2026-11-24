// SPDX-License-Identifier: Apache-2.0
#include "host/scene/closed_mesh.h"

#include <algorithm>
#include <vector>

namespace astroray::host {

bool polygons_closed(const uint32_t* corner_vert, const uint32_t* face_start, size_t face_count) {
  if (face_count == 0) return false;
  // One key per directed edge: the undirected edge (low, high) in the upper bits and the direction in
  // bit 0. In a closed oriented surface each undirected edge appears exactly twice, once each way, so
  // after sorting the keys come in pairs that share an edge and differ in direction, and no edge
  // repeats between pairs.
  std::vector<uint64_t> keys;
  keys.reserve(face_start[face_count]);
  for (size_t f = 0; f < face_count; ++f) {
    const uint32_t begin = face_start[f], end = face_start[f + 1];
    if (end - begin < 3) return false;
    for (uint32_t c = begin; c < end; ++c) {
      const uint64_t a = corner_vert[c], b = corner_vert[c + 1 == end ? begin : c + 1];
      if (a == b) return false;
      keys.push_back((std::min(a, b) << 32) | (std::max(a, b) << 1) | (a < b ? 0u : 1u));
    }
  }
  if (keys.size() % 2 != 0) return false;
  std::sort(keys.begin(), keys.end());
  for (size_t i = 0; i < keys.size(); i += 2) {
    if ((keys[i] >> 1) != (keys[i + 1] >> 1) || keys[i] == keys[i + 1]) return false;
    if (i + 2 < keys.size() && (keys[i + 2] >> 1) == (keys[i] >> 1)) return false;
  }
  return true;
}

}  // namespace astroray::host
