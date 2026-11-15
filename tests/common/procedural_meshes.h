// SPDX-License-Identifier: Apache-2.0
// Deterministic procedural meshes for traversal tests, built straight into host MeshRecords
// (no ingest path), and a transform helper.
// Ref: spike B's S1 is a displaced 1,000 x 500 grid of about 1.0 M triangles.
#pragma once
#include <cstdint>
#include <memory>

#include "host/scene/instances.h"
#include "host/scene/mesh.h"

namespace astroray::test {

// An nx x ny cell grid (unit cells, z = 0 plane) with two triangles per cell and each vertex
// displaced in z by a seeded hash plus a smooth swell: steep, edge-dense geometry.
std::shared_ptr<host::MeshRecord> displaced_grid(ar_key key, uint32_t nx, uint32_t ny, uint32_t seed);
// A unit-radius UV sphere (rings >= 2) and the cube [-1, 1]^3.
std::shared_ptr<host::MeshRecord> uv_sphere(ar_key key, uint32_t segments, uint32_t rings);
std::shared_ptr<host::MeshRecord> cube(ar_key key);

// Row-major 3 x 4: scale (sx, sy, sz) first, then rotation about the axis by `angle` radians,
// then translation.
ar_xform make_xform(float sx, float sy, float sz, float ax, float ay, float az, float angle, float tx, float ty,
                    float tz);

}  // namespace astroray::test
