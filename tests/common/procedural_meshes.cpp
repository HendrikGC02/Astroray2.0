// SPDX-License-Identifier: Apache-2.0
#include "procedural_meshes.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace astroray::test {
namespace {

uint32_t hash32(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}

// Triangles given as vertex-index triples; every corner is its own entry, like the ingest does.
std::shared_ptr<host::MeshRecord> finish(ar_key key, std::vector<float> pos, const std::vector<uint32_t>& tri) {
  auto t = std::make_shared<host::MeshTopology>();
  const uint32_t nt = uint32_t(tri.size() / 3);
  t->id = key, t->key = key;
  t->vertex_count = uint32_t(pos.size() / 3), t->corner_count = 3 * nt, t->face_count = nt;
  t->uv_layer_count = 1, t->motion_steps = 1;
  t->corner_vert = tri;
  t->tri_corners.resize(size_t(3) * nt), t->tri_face.resize(nt), t->tri_material.assign(nt, 0);
  t->face_start.resize(size_t(nt) + 1);
  for (uint32_t c = 0; c < 3 * nt; ++c) t->tri_corners[c] = c;
  for (uint32_t i = 0; i < nt; ++i) t->tri_face[i] = i, t->face_start[i] = 3 * i;
  t->face_start[nt] = 3 * nt;
  t->uv.assign(size_t(6) * nt, 0.f);
  auto rec = std::make_shared<host::MeshRecord>();
  rec->key = key;
  rec->corner_normals.assign(size_t(9) * nt, 0.f);
  for (int a = 0; a < 3; ++a) rec->bounds_min[a] = 1e30f, rec->bounds_max[a] = -1e30f;
  for (size_t v = 0; v < pos.size() / 3; ++v)
    for (int a = 0; a < 3; ++a) {
      rec->bounds_min[a] = std::min(rec->bounds_min[a], pos[3 * v + a]);
      rec->bounds_max[a] = std::max(rec->bounds_max[a], pos[3 * v + a]);
    }
  rec->positions = std::move(pos);
  rec->topo = t;
  return rec;
}

}  // namespace

std::shared_ptr<host::MeshRecord> displaced_grid(ar_key key, uint32_t nx, uint32_t ny, uint32_t seed) {
  std::vector<float> pos;
  pos.reserve(size_t(nx + 1) * (ny + 1) * 3);
  for (uint32_t j = 0; j <= ny; ++j)
    for (uint32_t i = 0; i <= nx; ++i) {
      const float h = float(hash32(hash32(i + seed) ^ (j * 0x9e3779b9u)) >> 8) * (1.0f / 16777216.0f);
      pos.push_back(float(i)), pos.push_back(float(j));
      pos.push_back(6.0f * (h - 0.5f) + 8.0f * std::sin(0.05f * float(i)) * std::cos(0.07f * float(j)));
    }
  std::vector<uint32_t> tri;
  tri.reserve(size_t(nx) * ny * 6);
  for (uint32_t j = 0; j < ny; ++j)
    for (uint32_t i = 0; i < nx; ++i) {
      const uint32_t a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
      tri.insert(tri.end(), {a, b, d, a, d, c});
    }
  return finish(key, std::move(pos), tri);
}

std::shared_ptr<host::MeshRecord> uv_sphere(ar_key key, uint32_t segments, uint32_t rings) {
  std::vector<float> pos;
  for (uint32_t r = 0; r <= rings; ++r) {
    const float th = 3.14159265358979f * float(r) / float(rings);
    for (uint32_t s = 0; s < segments; ++s) {
      const float ph = 6.28318530717959f * float(s) / float(segments);
      pos.push_back(std::sin(th) * std::cos(ph)), pos.push_back(std::sin(th) * std::sin(ph)), pos.push_back(std::cos(th));
    }
  }
  std::vector<uint32_t> tri;
  for (uint32_t r = 0; r < rings; ++r)
    for (uint32_t s = 0; s < segments; ++s) {
      const uint32_t a = r * segments + s, b = r * segments + (s + 1) % segments;
      const uint32_t c = a + segments, d = b + segments;
      if (r > 0) tri.insert(tri.end(), {a, c, b});  // the pole rows are degenerate: one triangle per segment
      if (r + 1 < rings) tri.insert(tri.end(), {b, c, d});
    }
  return finish(key, std::move(pos), tri);
}

std::shared_ptr<host::MeshRecord> cube(ar_key key) {
  std::vector<float> pos;
  for (uint32_t v = 0; v < 8; ++v)
    pos.push_back(v & 1 ? 1.f : -1.f), pos.push_back(v & 2 ? 1.f : -1.f), pos.push_back(v & 4 ? 1.f : -1.f);
  const uint32_t quads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}};
  std::vector<uint32_t> tri;
  for (const auto& q : quads) tri.insert(tri.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
  return finish(key, std::move(pos), tri);
}

ar_xform make_xform(float sx, float sy, float sz, float ax, float ay, float az, float angle, float tx, float ty,
                    float tz) {
  const float n = std::sqrt(ax * ax + ay * ay + az * az);
  ax /= n, ay /= n, az /= n;
  const float c = std::cos(angle), s = std::sin(angle), k = 1.0f - c;
  const float r[3][3] = {{c + ax * ax * k, ax * ay * k - az * s, ax * az * k + ay * s},
                         {ay * ax * k + az * s, c + ay * ay * k, ay * az * k - ax * s},
                         {az * ax * k - ay * s, az * ay * k + ax * s, c + az * az * k}};
  const float sc[3] = {sx, sy, sz}, t[3] = {tx, ty, tz};
  ar_xform x{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) x.m[4 * i + j] = r[i][j] * sc[j];
    x.m[4 * i + 3] = t[i];
  }
  return x;
}

}  // namespace astroray::test
