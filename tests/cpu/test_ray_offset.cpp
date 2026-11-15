// SPDX-License-Identifier: Apache-2.0
// Ray-origin offset: rays spawned from a tessellated sphere 1e4 m from the origin never hit
// their own triangle nor start below it, surfaces 1e-4 m apart are still found, and shadow
// segments end at their offset points. Expected values come from the geometry below, with
// the sphere's float positions rounded from double.
// Ref: docs/decisions/0002-kernel-architecture.md D16, task 038.
#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "device/cpu/embree_trace.h"
#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"
#include "kernel/geom/ray_offset.h"

using namespace astroray;
using namespace astroray::host;

namespace {

ar_xform translate(float x, float z = 0) {
  ar_xform t{};
  t.m[0] = t.m[5] = t.m[10] = 1.0f;
  t.m[3] = x, t.m[11] = z;
  return t;
}

// A triangle soup as a mesh record: corner c of triangle i is corner 3 i + c.
std::shared_ptr<MeshRecord> soup_record(ar_key key, uint32_t vertex_count, const std::vector<uint32_t>& tri_vert,
                                        std::vector<float> positions) {
  const uint32_t tris = uint32_t(tri_vert.size() / 3);
  auto topo = std::make_shared<MeshTopology>();
  topo->key = key;
  topo->vertex_count = vertex_count, topo->corner_count = 3 * tris, topo->face_count = tris, topo->motion_steps = 1;
  topo->corner_vert = tri_vert;
  for (uint32_t i = 0; i <= tris; ++i) topo->face_start.push_back(3 * i);
  for (uint32_t c = 0; c < 3 * tris; ++c) topo->tri_corners.push_back(c);
  for (uint32_t i = 0; i < tris; ++i) topo->tri_face.push_back(i), topo->tri_material.push_back(0);
  auto rec = std::make_shared<MeshRecord>();
  rec->key = key;
  rec->topo = topo;
  rec->positions = std::move(positions);
  return rec;
}

// A unit quad in the z = 0 plane.
std::shared_ptr<MeshRecord> quad_record(ar_key key) {
  return soup_record(key, 4, {0, 1, 2, 0, 2, 3}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
}

struct Rig {
  ContextState ctx;
  ThreadPool pool{ctx, 2};
  Ledger ledger;
  MeshStore meshes;
  EmbreeDevice device;
  InstanceTable table;
  std::unique_ptr<EmbreeScene> scene;
  void add(ar_key key, float x, float z) {
    Instance in;
    in.geom_key = key;
    in.mask = 0xFF;
    table.instances.push_back(in);
    table.xf.push_back(translate(x, z));
  }
  void build() { scene = std::make_unique<EmbreeScene>(device, meshes, table, ledger, pool); }
};

struct Rays {
  std::vector<float> ox, oy, oz, dx, dy, dz, tmin, tmax;
  std::vector<uint8_t> mask;
  void add(ar::Real3 o, ar::Real3 d, float far) {
    ox.push_back(o.x), oy.push_back(o.y), oz.push_back(o.z);
    dx.push_back(d.x), dy.push_back(d.y), dz.push_back(d.z);
    tmin.push_back(ar::kRayTmin), tmax.push_back(far), mask.push_back(0xFF);
  }
  RayBatch batch() const {
    return {ox.data(), oy.data(), oz.data(), dx.data(), dy.data(), dz.data(), tmin.data(), tmax.data()};
  }
  uint32_t size() const { return uint32_t(ox.size()); }
};

struct D3 {
  double x, y, z;
};
D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Rays from random points of a unit-radius UV sphere centred at (1e4, 0, 0), in random
// directions on both sides, each offset by offset_ray_origin and traced with tmin = 0.
void run_sphere(uint32_t nlat, uint32_t nrays) {
  const uint32_t nlon = 2 * nlat;
  const double kPi = 3.14159265358979323846;
  const D3 centre{1.0e4, 0, 0};
  std::vector<float> pos;
  std::vector<D3> vert;  // the float positions, widened
  auto push = [&](double x, double y, double z) {
    const float f[3] = {float(centre.x + x), float(centre.y + y), float(centre.z + z)};
    pos.insert(pos.end(), f, f + 3);
    vert.push_back({f[0], f[1], f[2]});
  };
  push(0, 0, 1);
  for (uint32_t i = 1; i < nlat; ++i)
    for (uint32_t j = 0; j < nlon; ++j) {
      const double th = kPi * i / nlat, ph = 2 * kPi * j / nlon;
      push(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
    }
  push(0, 0, -1);
  const uint32_t south = uint32_t(vert.size()) - 1;
  auto ring = [&](uint32_t i, uint32_t j) { return 1 + (i - 1) * nlon + (j % nlon); };
  std::vector<uint32_t> tv;
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    // Drop triangles that collapse when the vertices are rounded to float.
    const D3 n = cross(sub(vert[b], vert[a]), sub(vert[c], vert[a]));
    if (dot(n, n) > 0) tv.insert(tv.end(), {a, b, c});
  };
  for (uint32_t j = 0; j < nlon; ++j) {
    tri(0, ring(1, j), ring(1, j + 1));
    tri(south, ring(nlat - 1, j + 1), ring(nlat - 1, j));
    for (uint32_t i = 1; i + 1 < nlat; ++i) {
      tri(ring(i, j), ring(i + 1, j), ring(i + 1, j + 1));
      tri(ring(i, j), ring(i + 1, j + 1), ring(i, j + 1));
    }
  }
  const uint32_t ntri = uint32_t(tv.size() / 3);
  Rig r;
  r.meshes.install(soup_record(1, uint32_t(vert.size()), tv, pos));
  r.add(1, 0.0f, 0.0f);
  r.build();

  std::mt19937_64 rng(38);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  Rays rays;
  std::vector<uint32_t> own;
  std::vector<D3> nrm;  // per ray: the outward-oriented unit normal of its triangle, widened
  uint32_t below = 0;
  for (uint32_t k = 0; k < nrays; ++k) {
    const uint32_t t = uint32_t(rng() % ntri);
    const D3 v0 = vert[tv[3 * t]], v1 = vert[tv[3 * t + 1]], v2 = vert[tv[3 * t + 2]];
    D3 n = cross(sub(v1, v0), sub(v2, v0));
    const double nl = std::sqrt(dot(n, n));
    n = {n.x / nl, n.y / nl, n.z / nl};
    if (dot(n, sub(v0, centre)) < 0) n = {-n.x, -n.y, -n.z};
    double a = uni(rng), b = uni(rng);
    if (a + b > 1) a = 1 - a, b = 1 - b;
    // The hit point as a traversal would produce it: barycentric interpolation in float.
    const ar::Real3 p{float(v0.x + a * (v1.x - v0.x) + b * (v2.x - v0.x)),
                      float(v0.y + a * (v1.y - v0.y) + b * (v2.y - v0.y)),
                      float(v0.z + a * (v1.z - v0.z) + b * (v2.z - v0.z))};
    const double z = 2 * uni(rng) - 1, ph = 2 * kPi * uni(rng), s = std::sqrt(1 - z * z);
    const ar::Real3 w{float(s * std::cos(ph)), float(s * std::sin(ph)), float(z)};
    const ar::Real3 ng{float(n.x), float(n.y), float(n.z)};
    const ar::Real3 o = ar::offset_ray_origin(p, ng, w);
    const double side = dot(D3{w.x, w.y, w.z}, n) < 0 ? -1.0 : 1.0;
    if (!(side * dot(sub(D3{o.x, o.y, o.z}, v0), n) > 0)) ++below;
    rays.add(o, w, 1.0e30f);
    own.push_back(t);
  }
  std::vector<ar::HitRecord> hit(rays.size());
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
  uint32_t self_hits = 0;
  for (uint32_t k = 0; k < rays.size(); ++k)
    if (hit[k].inst == 0u && hit[k].prim == own[k]) ++self_hits;
  std::printf("triangles=%u rays=%u self_hits=%u below_surface=%u\n", ntri, rays.size(), self_hits, below);
  CHECK(self_hits == 0u);
  CHECK(below == 0u);
}

// Two parallel quads 1e-4 m apart at height z0 and z0 + 1e-4: rays leaving one toward the
// other still hit it (the offset is smaller than the gap).
void run_near_surface(float z0, uint32_t count, uint32_t& found, uint32_t& total) {
  Rig r;
  r.meshes.install(quad_record(1));
  r.add(1, 0.0f, z0);
  r.add(1, 0.0f, z0 + 1.0e-4f);
  r.build();
  const float gap = (z0 + 1.0e-4f) - z0;
  std::mt19937_64 rng(39);
  std::uniform_real_distribution<float> uni(0.0f, 1.0f);
  Rays rays;
  std::vector<uint32_t> want;
  for (uint32_t k = 0; k < count; ++k) {
    const bool up = (k & 1u) == 0;  // from the lower quad upward, or from the upper downward
    const ar::Real3 p{0.1f + 0.8f * uni(rng), 0.1f + 0.8f * uni(rng), up ? z0 : z0 + gap};
    const float c = 0.05f + 0.95f * uni(rng), ph = 6.2831853f * uni(rng), s = std::sqrt(1 - c * c);
    const ar::Real3 w{s * std::cos(ph), s * std::sin(ph), up ? c : -c};
    rays.add(ar::offset_ray_origin(p, ar::Real3{0, 0, 1}, w), w, 1.0e30f);
    want.push_back(up ? 1u : 0u);
  }
  std::vector<ar::HitRecord> hit(rays.size());
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
  for (uint32_t k = 0; k < rays.size(); ++k) found += hit[k].inst == want[k];
  total += rays.size();
}

void run_near(uint32_t count) {
  uint32_t found = 0, total = 0;
  run_near_surface(0.0f, count, found, total);  // fixed offset near zero
  run_near_surface(3.0f, count, found, total);  // 256 ulps at 3 m is 6e-5 m
  std::printf("near_surface_found=%u/%u\n", found, total);
  CHECK(found == total);
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("ray_offset/no self hits at 1e4 m") { run_sphere(50, 100000); }

TEST_CASE("ray_offset/near surface found") { run_near(5000); }

TEST_CASE("ray_offset/shadow segment") {
  // Two facing quads at z = 0 and 3: the segment between offset points is not occluded by
  // either; a blocker quad at z = 1.5 occludes it.
  const ar::Real3 p{0.5f, 0.5f, 0.0f}, q{0.5f, 0.5f, 3.0f};
  const ar::ShadowSegment s = ar::shadow_segment(p, ar::Real3{0, 0, 1}, q, ar::Real3{0, 0, -1});
  CHECK(s.origin.z > 0.0f);
  CHECK(s.origin.z + s.dir.z * s.tmax < 3.0f);
  CHECK(std::fabs(s.dir.z - 1.0f) < 1e-6f);
  Rays rays;
  rays.add(s.origin, s.dir, s.tmax);
  for (const bool blocked : {false, true}) {
    Rig r;
    r.meshes.install(quad_record(1));
    r.add(1, 0.0f, 0.0f);
    r.add(1, 0.0f, 3.0f);
    if (blocked) r.add(1, 0.0f, 1.5f);
    r.build();
    uint8_t occ = 7;
    trace_occluded(*r.scene, rays.batch(), 1, &occ, r.pool);
    CHECK(occ == (blocked ? 1 : 0));
  }
  const ar::ShadowSegment l = ar::shadow_segment_to_lamp(p, ar::Real3{0, 0, 1}, q);
  CHECK(l.origin.z > 0.0f);
  CHECK(std::fabs(l.origin.z + l.dir.z * l.tmax - 3.0f) < 1e-5f);
  const ar::Real3 past = ar::offset_past_surface(q, ar::Real3{0, 0, 1});
  CHECK(past.z > 3.0f);
}

}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("ray_offset/no self hits at 1e4 m") { run_sphere(159, 1000000); }

TEST_CASE("ray_offset/near surface found") { run_near(50000); }

}  // TEST_SUITE
