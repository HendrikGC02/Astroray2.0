// SPDX-License-Identifier: Apache-2.0
// The closed-mesh test a medium-bearing mesh must pass: every edge belongs to exactly two triangles
// that traverse it in opposite directions, so the surface has an inside and a consistent orientation.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D20 (closed, consistently wound at sync).
#pragma once
#include <cstddef>
#include <cstdint>

namespace astroray::host {

// `tri_vertex` holds 3 * triangle_count vertex indices (corner_vert looked up through the triangle's
// corners). Vertices are identified by index, so a mesh whose faces share positions but not indices
// is open. A degenerate edge, an edge used by more than two triangles, two triangles that traverse
// an edge the same way, and an empty mesh are all not closed.
bool triangles_closed(const uint32_t* tri_vertex, size_t triangle_count);

}  // namespace astroray::host
