// SPDX-License-Identifier: Apache-2.0
// Native triangulation of Blender-style polygon meshes (0007 d10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace astroray::host {

// Triangles of every face as corner indices (3 per triangle, indices into
// corner_vert). Face f owns corners face_start[f] .. face_start[f+1]-1;
// face_start has face_count + 1 entries. positions is xyz per vertex.
// Triangles are copied, quads follow Blender's diagonal rule, faces of 5 or more
// corners are ear-clipped on the polygon projected along its Newell normal.
// Inputs are assumed validated (035). Faces with fewer than 3 corners emit nothing.
std::vector<uint32_t> triangulate(const float* positions, const uint32_t* corner_vert,
                                  const uint32_t* face_start, size_t face_count);

}  // namespace astroray::host
