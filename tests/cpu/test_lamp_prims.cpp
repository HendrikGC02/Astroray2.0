// SPDX-License-Identifier: Apache-2.0
// Lamp primitives in Embree: disk, rectangle and sphere lamps hit at analytic t, a delta point
// lamp has no surface, the object transform of a lamp instance is ignored and no lamp blocks a
// shadow ray. Expected values are typed from the geometry (plane and sphere equations).
// Ref: docs/decisions/0002-kernel-architecture.md D11, 0004 D8; pbrt-v4 Disk::BasicIntersect.
#include <doctest.h>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

#include "device/cpu/embree_trace.h"
#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"
#include "light/light.h"

using namespace astroray;
using namespace astroray::host;

namespace {

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, 1.0f); }

struct Rig {
  ContextState ctx;
  ThreadPool pool{ctx, 2};
  Ledger ledger;
  MeshStore meshes;
  EmbreeDevice device;
  InstanceTable table;
  std::vector<ar::LightRecord> lights;
  std::unique_ptr<EmbreeScene> scene;

  void add(const ar::LightRecord& rec) {
    Instance in;
    in.light_index = uint32_t(lights.size());
    in.mask = visibility_mask(kLampRayVisibility);
    table.instances.push_back(in);
    ar_xform wild{};  // the object transform must not matter
    wild.m[0] = 4.0f, wild.m[5] = 0.5f, wild.m[10] = 9.0f, wild.m[3] = 30.0f, wild.m[7] = 30.0f, wild.m[11] = -30.0f;
    table.xf.push_back(wild);
    lights.push_back(rec);
  }
  void build() { scene = std::make_unique<EmbreeScene>(device, meshes, table, lights, ledger, pool); }

  // Closest hit of the ray o + t d (a camera ray), t within [0, tmax].
  ar::HitRecord trace(ar::Real3 o, ar::Real3 d, float tmax = 100.0f) {
    const float ox = o.x, oy = o.y, oz = o.z, dx = d.x, dy = d.y, dz = d.z, t0 = 0.0f;
    const RayBatch b{&ox, &oy, &oz, &dx, &dy, &dz, &t0, &tmax};
    const uint8_t mask = 0x01;
    ar::HitRecord h{};
    trace_closest(*scene, b, &mask, 1, &h, pool);
    return h;
  }
  bool occluded(ar::Real3 o, ar::Real3 d, float tmax = 100.0f) {
    const float ox = o.x, oy = o.y, oz = o.z, dx = d.x, dy = d.y, dz = d.z, t0 = 0.0f;
    const RayBatch b{&ox, &oy, &oz, &dx, &dy, &dz, &t0, &tmax};
    uint8_t occ = 0;
    trace_occluded(*scene, b, 1, &occ, pool);
    return occ != 0;
  }
};

const ar::Real3 kX{1, 0, 0}, kY{0, 1, 0}, kDown{0, 0, -1};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("lamp_prims/disk analytic t") {
  Rig r;
  // Radius 2 at (1, 2, 3) in the XY plane, then the same disk tilted: Y' = (0, c, s), Z' = X x Y'.
  r.add(ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{1, 2, 3}, kX, kY, 2.0f, 0.0f, 1.0f, white()));
  const float c = 0.5f, sn = 0.8660254f;  // 60 degrees about X
  r.add(ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{10, 2, 3}, kX, ar::Real3{0, c, sn}, 2.0f, 0.0f, 1.0f, white()));
  r.build();
  // Flat disk: the plane z = 3 seen from z = 10.
  ar::HitRecord h = r.trace(ar::Real3{1, 2, 10}, kDown);
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 7.0f) < 1e-6f);
  h = r.trace(ar::Real3{2.99f, 2, 10}, kDown);  // just inside the rim
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 7.0f) < 1e-6f);
  CHECK(r.trace(ar::Real3{3.01f, 2, 10}, kDown).inst == ar::kNoInstance);  // just outside
  h = r.trace(ar::Real3{1, 2, -4}, ar::Real3{0, 0, 1});  // from behind: geometry only, shading decides
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 7.0f) < 1e-6f);
  CHECK(r.trace(ar::Real3{1, 2, 10}, kDown, 6.9f).inst == ar::kNoInstance);  // tmax before the disk
  CHECK(r.trace(ar::Real3{1, 5, 3}, kX).inst == ar::kNoInstance);              // parallel to the plane
  // Tilted disk, normal n = (0, -s, c) (the cross product of X and Y'), centre (10, 2, 3): the
  // vertical ray at (10, 2 + 0.5, .) meets the plane where (p - centre) . n = 0, i.e. z = 3 + 0.5 s / c.
  h = r.trace(ar::Real3{10, 2.5f, 10}, kDown);
  CHECK(h.inst == 1u);
  CHECK(std::fabs(h.t - (10.0f - (3.0f + 0.5f * sn / c))) < 1e-6f * 7.0f);
  // The lamp instance's object transform (scale 4, shifted by 30) does not move the primitive.
  CHECK(r.trace(ar::Real3{34, 32, 10}, kDown).inst == ar::kNoInstance);
}

TEST_CASE("lamp_prims/sphere analytic t") {
  Rig r;
  r.add(ar::light_make(ar::LightType::SPHERE, ar::Real3{1, 2, 3}, 1.5f, 1.0f, white()));
  r.add(ar::light_make(ar::LightType::SPHERE, ar::Real3{20, 2, 3}, 0.5f, 1.0f, white()));
  r.build();
  ar::HitRecord h = r.trace(ar::Real3{1, 2, 10}, kDown);
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 5.5f) < 1e-6f * 5.5f);
  h = r.trace(ar::Real3{1.9f, 2, 10}, kDown);  // z = 3 + sqrt(r^2 - 0.81)
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - (7.0f - std::sqrt(1.5f * 1.5f - 0.81f))) < 1e-5f);
  CHECK(r.trace(ar::Real3{2.6f, 2, 10}, kDown).inst == ar::kNoInstance);
  h = r.trace(ar::Real3{1, 2, 3}, kDown);  // from the centre, out through the surface
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 1.5f) < 1e-6f * 1.5f);
  h = r.trace(ar::Real3{20, 2, 10}, kDown);
  CHECK(h.inst == 1u);
  CHECK(std::fabs(h.t - 6.5f) < 1e-6f * 6.5f);
}

TEST_CASE("lamp_prims/rectangle analytic t") {
  Rig r;
  r.add(ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{1, 2, 3}, kX, kY, 2.0f, 1.0f, 1.0f, white()));
  r.build();
  ar::HitRecord h = r.trace(ar::Real3{1.9f, 2.4f, 10}, kDown);  // inside: x in [0, 2], y in [1.5, 2.5]
  CHECK(h.inst == 0u);
  CHECK(std::fabs(h.t - 7.0f) < 1e-6f);
  CHECK(r.trace(ar::Real3{2.1f, 2.0f, 10}, kDown).inst == ar::kNoInstance);  // beyond the width
  CHECK(r.trace(ar::Real3{1.0f, 2.6f, 10}, kDown).inst == ar::kNoInstance);  // beyond the height
}

TEST_CASE("lamp_prims/delta point lamp has no primitive") {
  Rig r;
  r.add(ar::light_make(ar::LightType::POINT, ar::Real3{1, 2, 3}, 0.0f, 1.0f, white()));
  r.build();
  CHECK(r.trace(ar::Real3{1, 2, 10}, kDown).inst == ar::kNoInstance);
}

TEST_CASE("lamp_prims/lamps never block shadows") {
  Rig r;
  r.add(ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{1, 2, 3}, kX, kY, 2.0f, 0.0f, 1.0f, white()));
  r.add(ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{10, 2, 3}, kX, kY, 2.0f, 1.0f, 1.0f, white()));
  r.add(ar::light_make(ar::LightType::SPHERE, ar::Real3{20, 2, 3}, 1.5f, 1.0f, white()));
  r.build();
  for (float x : {1.0f, 10.0f, 20.0f}) {
    CHECK(r.trace(ar::Real3{x, 2, 10}, kDown).inst != ar::kNoInstance);  // the lamp is there for a camera ray
    CHECK_FALSE(r.occluded(ar::Real3{x, 2, 10}, kDown));                  // and invisible to a shadow ray
  }
}

TEST_CASE("lamp_prims/invalid lamp instances throw") {
  using doctest::Contains;
  auto lamp = [](uint32_t light) {
    Instance in;
    in.light_index = light;
    in.mask = visibility_mask(kLampRayVisibility);
    return in;
  };
  auto build = [&](const Instance& in, std::vector<ar::LightRecord> lights) {
    Rig r;
    r.table.instances.push_back(in);
    r.table.xf.push_back(ar_xform{});
    r.lights = std::move(lights);
    r.build();
  };
  // No light 3: never a silently missing lamp.
  CHECK_THROWS_WITH_AS(build(lamp(3), {}), Contains("instance 0 names light 3"), std::runtime_error);
  // An emissive triangle is a light but not a lamp.
  const ar::LightRecord tri = ar::light_make_triangle(ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 0, 0, 0, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(lamp(0), {tri}), Contains("instance 0 names light 0 which is not a lamp"), std::runtime_error);
  // A point lamp with a radius is refused by light_problem and sampled as a delta: not a sphere here.
  const ar::LightRecord point_r = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, 0.5f, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(lamp(0), {point_r}), Contains("instance 0 names light 0 which is not a lamp"), std::runtime_error);
  // 0004 D8: a lamp instance with the shadow bit is a bug, not something to clear quietly.
  Instance shadowed = lamp(0);
  shadowed.mask = uint8_t(shadowed.mask | ar::kShadowMaskBit);
  const ar::LightRecord disk = ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{0, 0, 0}, kX, kY, 1.0f, 0.0f, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(shadowed, {disk}), Contains("instance 0 is a lamp whose mask carries the shadow bit"), std::runtime_error);
}

}  // TEST_SUITE
