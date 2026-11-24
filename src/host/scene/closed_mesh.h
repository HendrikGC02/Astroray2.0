// SPDX-License-Identifier: Apache-2.0
// The closed-mesh test a medium-bearing mesh must pass: every edge belongs to exactly two faces
// that traverse it in opposite directions, so the surface has an inside and a consistent orientation.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D20 (closed, consistently wound at sync).
#pragma once
#include <cstddef>
#include <cstdint>

namespace astroray::host {

// `corner_vert` holds the face corners' vertex indices (each below 2^31) and `face_start` has
// face_count + 1 entries, the last being the corner count. The edges are the polygon's own, so a
// zero-area face that triangulation drops still counts. Vertices are identified by index, so a mesh
// whose faces share positions but not indices is open. A degenerate edge, an edge used by more than
// two faces, two faces that traverse an edge the same way, a face of fewer than 3 corners and an
// empty mesh are all not closed. Sorts one array of edges: O(C log C).
bool polygons_closed(const uint32_t* corner_vert, const uint32_t* face_start, size_t face_count);

}  // namespace astroray::host
