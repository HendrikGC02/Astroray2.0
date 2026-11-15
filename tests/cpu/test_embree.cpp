// SPDX-License-Identifier: Apache-2.0
// Embree traversal: analytic hits on a unit quad, misses, the instance-mask fixture and the
// shadow-bit rule of occlusion. Expected values are typed from the geometry below.
// Ref: docs/decisions/0002-kernel-architecture.md D11 (mask fixture, opaque shadow rule).
#include <doctest.h>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "device/cpu/embree_trace.h"
#include "host/fault.h"
#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr ar_key kQuadKey = 5;

// A unit quad in the z = 0 plane, corners (0,0) (1,0) (1,1) (0,1), as triangles
// (0,1,2) and (0,2,3).
std::shared_ptr<MeshRecord> quad_record(ar_key key = kQuadKey, std::vector<uint32_t> corner_vert = {0, 1, 2, 3},
                                        std::vector<float> positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0},
                                        uint32_t steps = 1) {
  auto topo = std::make_shared<MeshTopology>();
  topo->key = key;
  topo->vertex_count = 4, topo->corner_count = 4, topo->face_count = 1, topo->motion_steps = steps;
  topo->corner_vert = std::move(corner_vert);
  topo->face_start = {0, 4};
  topo->tri_corners = {0, 1, 2, 0, 2, 3};
  topo->tri_face = {0, 0};
  topo->tri_material = {0, 0};
  auto rec = std::make_shared<MeshRecord>();
  rec->key = key;
  rec->topo = topo;
  rec->positions = std::move(positions);
  return rec;
}

ar_xform translate(float x, float z = 0) {
  ar_xform t{};
  t.m[0] = t.m[5] = t.m[10] = 1.0f;
  t.m[3] = x, t.m[11] = z;
  return t;
}

struct Rig {
  ContextState ctx;
  ThreadPool pool{ctx, 2};
  Ledger ledger;
  MeshStore meshes;
  EmbreeDevice device;
  InstanceTable table;
  std::unique_ptr<EmbreeScene> scene;
  Rig() { meshes.install(quad_record()); }
  void add(uint8_t mask, float x, float z = 0, ar_key key = kQuadKey) {
    Instance in;
    in.geom_key = key;
    in.mask = mask;
    table.instances.push_back(in);
    table.xf.push_back(translate(x, z));
  }
  void build() { scene = std::make_unique<EmbreeScene>(device, meshes, table, ledger, pool); }
};

// Rays that start above (x, y) at z = 2 and look straight down.
struct Rays {
  std::vector<float> ox, oy, oz, dx, dy, dz, tmin, tmax;
  std::vector<uint8_t> mask;
  void add(float x, float y, uint8_t m, float far = 100.0f) {
    ox.push_back(x), oy.push_back(y), oz.push_back(2.0f);
    dx.push_back(0), dy.push_back(0), dz.push_back(-1.0f);
    tmin.push_back(0), tmax.push_back(far), mask.push_back(m);
  }
  RayBatch batch() const { return {ox.data(), oy.data(), oz.data(), dx.data(), dy.data(), dz.data(), tmin.data(), tmax.data()}; }
  uint32_t size() const { return uint32_t(ox.size()); }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("embree/quad barycentrics") {
  Rig r;
  r.add(0xFF, 0.0f);
  r.build();
  Rays rays;
  rays.add(0.75f, 0.25f, 0xFF);  // triangle 0: u = x - y, v = y
  rays.add(0.25f, 0.75f, 0xFF);  // triangle 1: u = x, v = y - x
  std::vector<ar::HitRecord> hit(rays.size());
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
  CHECK(hit[0].inst == 0u);
  CHECK(hit[0].prim == 0u);
  CHECK(std::fabs(hit[0].t - 2.0f) < 1e-6f);
  CHECK(std::fabs(hit[0].u - 0.5f) < 1e-6f);
  CHECK(std::fabs(hit[0].v - 0.25f) < 1e-6f);
  CHECK(hit[1].inst == 0u);
  CHECK(hit[1].prim == 1u);
  CHECK(std::fabs(hit[1].t - 2.0f) < 1e-6f);
  CHECK(std::fabs(hit[1].u - 0.25f) < 1e-6f);
  CHECK(std::fabs(hit[1].v - 0.5f) < 1e-6f);
}

TEST_CASE("embree/instance transform and miss") {
  Rig r;
  r.add(0xFF, 3.0f, 1.0f);  // the quad moved to x in [3, 4], z = 1
  r.build();
  CHECK(r.ledger.category_total(LedgerCategory::ACCELERATION) == r.scene->acceleration_bytes());
  CHECK(r.scene->acceleration_bytes() > 0);
  Rays rays;
  rays.add(3.5f, 0.5f, 0xFF);  // hits at t = 1
  rays.add(0.5f, 0.5f, 0xFF);  // beside the instance
  rays.add(3.5f, 0.5f, 0xFF, 0.5f);  // tmax before the surface
  std::vector<ar::HitRecord> hit(rays.size());
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
  CHECK(hit[0].inst == 0u);
  CHECK(std::fabs(hit[0].t - 1.0f) < 1e-6f);
  CHECK(hit[1].inst == ~0u);
  CHECK(hit[2].inst == ~0u);
}

TEST_CASE("embree/corner lookup") {
  // The same quad with corners {2,3,0,1}: corner c is vertex corner_vert[c], so using corner
  // indices as vertex indices would build a different triangle.
  Rig r;
  r.meshes.install(quad_record(6, {2, 3, 0, 1}, {1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0}));
  r.add(0xFF, 0.0f, 0, 6);
  r.build();
  Rays rays;
  rays.add(0.75f, 0.25f, 0xFF);
  rays.add(0.25f, 0.75f, 0xFF);
  std::vector<ar::HitRecord> hit(rays.size());
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
  CHECK(hit[0].prim == 0u);
  CHECK(std::fabs(hit[0].u - 0.5f) < 1e-6f);
  CHECK(std::fabs(hit[0].v - 0.25f) < 1e-6f);
  CHECK(hit[1].prim == 1u);
  CHECK(std::fabs(hit[1].u - 0.25f) < 1e-6f);
  CHECK(std::fabs(hit[1].v - 0.5f) < 1e-6f);
}

TEST_CASE("embree/representative motion step") {
  // Three steps with the quad at z = 5, 0, -5: the representative step (S - 1) / 2 = 1 is
  // the one at z = 0, so a ray from z = 2 hits at t = 2.
  std::vector<float> pos;
  for (float z : {5.0f, 0.0f, -5.0f})
    for (float v : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f}) pos.push_back(v);
  for (size_t s = 0; s < 3; ++s)
    for (size_t k = 0; k < 4; ++k) pos[(s * 4 + k) * 3 + 2] = (s == 0 ? 5.0f : s == 1 ? 0.0f : -5.0f);
  Rig r;
  r.meshes.install(quad_record(7, {0, 1, 2, 3}, pos, 3));
  r.add(0xFF, 0.0f, 0, 7);
  r.build();
  Rays rays;
  rays.add(0.75f, 0.25f, 0xFF);
  std::vector<ar::HitRecord> hit(1);
  trace_closest(*r.scene, rays.batch(), rays.mask.data(), 1, hit.data(), r.pool);
  CHECK(hit[0].inst == 0u);
  CHECK(std::fabs(hit[0].t - 2.0f) < 1e-6f);
}

TEST_CASE("embree/missing geometry key") {
  Rig r;
  Instance in;
  in.geom_key = 999;
  in.mask = 1;
  r.table.instances.push_back(in);
  r.table.xf.push_back(translate(0));
  CHECK_THROWS_AS(r.build(), std::runtime_error);
}

TEST_CASE("embree/mask fixture") {
  // Instance i carries only mask bit i and sits at x = 2 i; a ray of mask m aimed at
  // instance i must hit it exactly when m has bit i.
  Rig r;
  for (uint32_t i = 0; i < 6; ++i) r.add(uint8_t(1u << i), 2.0f * float(i));
  r.build();
  const uint8_t masks[] = {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 0x0F, 0x2A, 0x3F, 0x00, 0xC0};
  for (uint8_t m : masks) {
    Rays rays;
    for (uint32_t i = 0; i < 6; ++i) rays.add(2.0f * float(i) + 0.5f, 0.5f, m);
    std::vector<ar::HitRecord> hit(rays.size());
    trace_closest(*r.scene, rays.batch(), rays.mask.data(), rays.size(), hit.data(), r.pool);
    for (uint32_t i = 0; i < 6; ++i) {
      INFO("mask " << unsigned(m) << " instance " << i);
      if ((m >> i) & 1u) CHECK(hit[i].inst == i);
      else CHECK(hit[i].inst == ~0u);
    }
  }
}

TEST_CASE("embree/occlusion ignores instances without the shadow bit") {
  Rig r;
  r.add(1u << 0, 0.0f);          // camera only
  r.add(1u << 4, 2.0f);          // shadow
  r.add(0xFF, 4.0f);             // everything
  r.build();
  Rays rays;
  rays.add(0.5f, 0.5f, 0);       // over the camera-only instance
  rays.add(2.5f, 0.5f, 0);       // over the shadow instance
  rays.add(4.5f, 0.5f, 0);       // over the all-bits instance
  rays.add(2.5f, 0.5f, 0, 1.0f); // shadow instance beyond tmax (hit at t = 2)
  rays.add(6.5f, 0.5f, 0);       // over nothing
  std::vector<uint8_t> occ(rays.size(), 7);
  trace_occluded(*r.scene, rays.batch(), rays.size(), occ.data(), r.pool);
  CHECK(occ[0] == 0);
  CHECK(occ[1] == 1);
  CHECK(occ[2] == 1);
  CHECK(occ[3] == 0);
  CHECK(occ[4] == 0);
}

}  // TEST_SUITE
