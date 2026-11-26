// SPDX-License-Identifier: Apache-2.0
// route_hit: the next stage for every class of hit and miss, from tables typed by hand.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11 (route_hit); 0005 item 17 (EXTENSION);
// 0006 D16 (the medium-stack depth in path-flag bits 12-14).
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "stages/route_hit.h"

using ar::Stage;

namespace {

constexpr uint32_t kLight = ar::PATH_KIND;

// Instances: 0 plain, 1 lamp, 2 extension material, 3 no material slice, 4 non-opaque only, 5 lamp with an extension material,
// 6 plain and bearing a medium. One mesh with two triangles; their material slots are 0 and 1.
struct SceneRig {
  std::vector<int32_t> tri_material{0, 1};
  std::vector<ar::DeviceMeshView> meshes{1};
  std::vector<ar::DeviceInstanceRecord> instances{7};
  // Pool entries 0..2 -> header 0 (Common), 1 (EXTENSION), 2 (NON_OPAQUE).
  std::vector<uint32_t> slots{0, 1, 2};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}, {0, 0, 0, ar::NON_OPAQUE}};
  ar::SceneData scene{};
  ar::KernelGlobals kg{};

  explicit SceneRig(bool with_materials = true) {
    meshes[0] = {};
    meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material.data()));
    meshes[0].triangle_count = 2;
    for (auto& r : instances) r = {}, r.light_index = ar::kNoLightIndex;
    instances[0].material_offset = 0, instances[0].material_count = 1;
    instances[1].light_index = 7;
    instances[2].material_offset = 1, instances[2].material_count = 1;
    instances[3].material_count = 0;
    instances[4].material_offset = 2, instances[4].material_count = 1;
    instances[5].light_index = 8, instances[5].material_offset = 1, instances[5].material_count = 1;
    instances[6].material_offset = 0, instances[6].material_count = 1, instances[6].medium = 1;
    scene.instances = instances.data(), scene.instance_count = uint32_t(instances.size());
    scene.meshes = meshes.data(), scene.mesh_count = 1;
    scene.medium_count = 1;
    if (with_materials) {
      scene.material_slots = slots.data();
      scene.material_headers = headers.data();
      scene.material_header_count = uint32_t(headers.size());
    }
    kg.scene = &scene;
  }
  Stage route(uint32_t inst, uint32_t prim, uint32_t flags) const {
    const ar::HitRecord h{1.0f, prim, inst, 0.25f, 0.25f};
    return ar::route_hit(kg, h, flags);
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("route_hit/stack depth routes to shade_volume") {
  // A segment with a non-empty medium stack ran inside a medium, whatever it ended on: every hit, of
  // either path kind, on either triangle, of an instance with or without a medium, a lamp or an
  // extension material, goes to the volume stage; the stack's highest entry names the medium there.
  SceneRig r;
  for (uint32_t depth = 1; depth <= 4; ++depth)
    for (uint32_t inst = 0; inst < 7; ++inst)
      for (uint32_t prim = 0; prim < 2; ++prim)
        for (const uint32_t kind : {0u, kLight}) {
          INFO("depth " << depth << " instance " << inst << " triangle " << prim << " kind " << kind);
          CHECK(r.route(inst, prim, ar::set_medium_depth(kind, depth)) == Stage::SHADE_VOLUME);
        }
  // At depth 0 a medium-bearing instance routes as any other: its own medium is entered or left only
  // when shade_surface crosses it.
  CHECK(r.route(6, 0, 0u) == Stage::SHADE_SURFACE);
  CHECK(r.route(6, 1, kLight) == Stage::SHADE_LIGHT_PATH);
  // A miss is a miss at any depth (a closed hull always ends a segment inside it).
  const ar::HitRecord miss{5.0f, ~0u, ar::kNoInstance, 0, 0};
  CHECK(ar::route_hit(r.kg, miss, ar::set_medium_depth(0u, 2u)) == Stage::SHADE_BACKGROUND);
  CHECK(ar::route_hit(r.kg, miss, ar::set_medium_depth(kLight, 2u)) == Stage::TERMINATED);
  // Other flag bits, the crossing count among them, do not route there.
  CHECK(r.route(0, 0, ar::FRONT_FACE | ar::set_crossings(0u, 1023u)) == Stage::SHADE_SURFACE);
}


TEST_CASE("route_hit/camera path miss shades background") {
  SceneRig r;
  const ar::HitRecord miss{5.0f, ~0u, ar::kNoInstance, 0, 0};
  CHECK(ar::route_hit(r.kg, miss, 0u) == Stage::SHADE_BACKGROUND);
}

TEST_CASE("route_hit/light path miss terminates") {
  SceneRig r;
  const ar::HitRecord miss{5.0f, ~0u, ar::kNoInstance, 0, 0};
  CHECK(ar::route_hit(r.kg, miss, kLight) == Stage::TERMINATED);
}

TEST_CASE("route_hit/lamp instance shades light") {
  SceneRig r;
  CHECK(r.route(1, 0, 0u) == Stage::SHADE_LIGHT);
  CHECK(r.route(1, 0, kLight) == Stage::SHADE_LIGHT);
}

TEST_CASE("route_hit/lamp wins over an extension material") {
  // 0002 D8: a lamp hit is pass-through whatever material the lamp carries.
  SceneRig r;
  CHECK(r.route(5, 0, 0u) == Stage::SHADE_LIGHT);
  CHECK(r.route(5, 0, kLight) == Stage::SHADE_LIGHT);
}

TEST_CASE("route_hit/extension material routes to the ext identities") {
  SceneRig r;
  CHECK(r.route(2, 0, 0u) == Stage::SHADE_SURFACE_EXT);
  CHECK(r.route(2, 0, kLight) == Stage::SHADE_LIGHT_PATH_EXT);
}

TEST_CASE("route_hit/common material routes to surface") {
  SceneRig r;
  CHECK(r.route(0, 0, 0u) == Stage::SHADE_SURFACE);
  CHECK(r.route(0, 0, kLight) == Stage::SHADE_LIGHT_PATH);
  // Only EXTENSION changes the route: a non-opaque material and an empty slice (the default material)
  // are Common.
  CHECK(r.route(4, 0, 0u) == Stage::SHADE_SURFACE);
  CHECK(r.route(3, 0, kLight) == Stage::SHADE_LIGHT_PATH);
}

TEST_CASE("route_hit/flags other than the path kind and the stack depth are ignored") {
  SceneRig r;
  CHECK(r.route(2, 0, ar::FRONT_FACE | ar::LAST_BOUNCE_SPECULAR) == Stage::SHADE_SURFACE_EXT);
  CHECK(r.route(0, 0, ar::FRONT_FACE | kLight) == Stage::SHADE_LIGHT_PATH);
}

TEST_CASE("route_hit/no material headers means every material is Common") {
  SceneRig r(false);
  CHECK(r.route(2, 0, 0u) == Stage::SHADE_SURFACE);
  CHECK(r.route(2, 0, kLight) == Stage::SHADE_LIGHT_PATH);
  CHECK(r.route(1, 0, 0u) == Stage::SHADE_LIGHT);
}

}  // TEST_SUITE
