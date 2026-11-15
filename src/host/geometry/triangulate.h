// SPDX-License-Identifier: Apache-2.0
// Native triangulation of Blender-style polygon meshes (0007 d10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace astroray::host {

// Result of triangulate(): triangles as corner indices (3 per triangle, indices into
// corner_vert), and the source face of every triangle.
struct Triangulation {
  std::vector<uint32_t> corners;
  std::vector<uint32_t> face;
  uint32_t skipped_zero_area = 0;  // faces of 5+ corners with zero area: no triangles (ruling 67)
  uint32_t fan_fallback = 0;       // non-degenerate faces ear clipping failed on: fan from corner 0 (ruling 67)
};

// Face f owns corners face_start[f] .. face_start[f+1]-1; face_start has face_count + 1
// entries. positions is xyz per vertex. Triangles are copied, quads follow Blender's
// diagonal rule, faces of 5 or more corners are ear-clipped on the polygon projected along
// its Newell normal. Never throws for geometry: a zero-area face emits nothing and an
// unclippable face becomes a fan, each counted for the caller to report.
// Inputs are assumed validated (035). Faces with fewer than 3 corners emit nothing.
Triangulation triangulate(const float* positions, const uint32_t* corner_vert,
                          const uint32_t* face_start, size_t face_count);

}  // namespace astroray::host
