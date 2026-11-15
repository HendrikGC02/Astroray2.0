// SPDX-License-Identifier: Apache-2.0
// Embree traversal: analytic hits on a unit quad, misses, the instance-mask fixture and the
// shadow-bit rule of occlusion. Expected values are typed from the geometry below.
// Ref: docs/decisions/0002-kernel-architecture.md D11 (mask fixture, opaque shadow rule).
#include <doctest.h>

#include <cmath>
#include <memory>
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
std::shared_ptr<MeshRecord> quad_record() {
  auto topo = std::make_shared<MeshTopology>();
  topo->key = kQuadKey;
  topo->vertex_count = 4, topo->corner_count = 4, topo->face_count = 1, topo->motion_steps = 1;
  topo->corner_vert = {0, 1, 2, 3};
  topo->face_start = {0, 4};
  topo->tri_corners = {0, 1, 2, 0, 2, 3};
  topo->tri_face = {0, 0};
  topo->tri_material = {0, 0};
  auto rec = std::make_shared<MeshRecord>();
  rec->key = kQuadKey;
  rec->topo = topo;
  rec->positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
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
  void add(uint8_t mask, float x, float z = 0) {
    Instance in;
    in.geom_key = kQuadKey;
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
