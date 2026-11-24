// SPDX-License-Identifier: Apache-2.0
// route_hit: the next stage for every class of hit and miss, from tables typed by hand.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11 (route_hit); 0005 item 17 (EXTENSION).
#include <doctest.h>

#include <cstdint>
#include <vector>

#include "stages/route_hit.h"
#include "stages/shade_common.h"

using ar::Stage;

namespace {

constexpr uint32_t kLight = ar::PATH_KIND;
// A ray direction for hits that are not tested for their side.
const ar::Real3 kDown{0, 0, -1}, kUp{0, 0, 1};

// Instances: 0 plain, 1 lamp, 2 extension material, 3 no material slice, 4 non-opaque only, 5 lamp with an extension material.
// One mesh with two triangles; their material slots are 0 and 1.
struct SceneRig {
  std::vector<int32_t> tri_material{0, 1};
  std::vector<ar::DeviceMeshView> meshes{1};
  std::vector<ar::DeviceInstanceRecord> instances{6};
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
    scene.instances = instances.data(), scene.instance_count = uint32_t(instances.size());
    scene.meshes = meshes.data(), scene.mesh_count = 1;
    if (with_materials) {
      scene.material_slots = slots.data();
      scene.material_headers = headers.data();
      scene.material_header_count = uint32_t(headers.size());
    }
    kg.scene = &scene;
  }
  Stage route(uint32_t inst, uint32_t prim, uint32_t flags) const {
    const ar::HitRecord h{1.0f, prim, inst, 0.25f, 0.25f};
    return ar::route_hit(kg, h, kDown, flags);
  }
};

// One mesh of two triangles over the same three vertices, wound opposite ways: triangle 0 faces +z, triangle 1
// faces -z. Instances 0 and 1 bear a medium (1 is mirrored in x, determinant -1), 2 bears a medium and an
// extension material, 3 bears none. Every pointer a hit reads is real, so shade_context_geometry can run
// on the same tables.
struct MediumRig {
  std::vector<int32_t> tri_material{0, 0};
  std::vector<uint32_t> tri_vertex{0, 1, 2, 0, 2, 1};
  std::vector<uint32_t> tri_corners{0, 1, 2, 0, 2, 1};
  std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::vector<float> corner_normals{0, 0, 1, 0, 0, 1, 0, 0, 1};
  std::vector<ar::DeviceMeshView> meshes{1};
  std::vector<ar::DeviceInstanceRecord> instances{4};
  std::vector<uint32_t> slots{0, 1, 0};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}};
  ar::SceneData scene{};
  ar::KernelGlobals kg{};
  MediumRig() {
    const auto addr = [](const auto& v) { return uint64_t(reinterpret_cast<uintptr_t>(v.data())); };
    meshes[0] = {};
    meshes[0].tri_material = addr(tri_material), meshes[0].tri_vertex = addr(tri_vertex);
    meshes[0].tri_corners = addr(tri_corners), meshes[0].positions = addr(positions);
    meshes[0].corner_normals = addr(corner_normals);
    meshes[0].triangle_count = 2, meshes[0].vertex_count = 3, meshes[0].corner_count = 3;
    for (uint32_t i = 0; i < 4; ++i) {
      instances[i] = {};
      instances[i].light_index = ar::kNoLightIndex;
      instances[i].material_offset = i == 2 ? 1u : 0u, instances[i].material_count = 1;
      instances[i].object_to_world[0] = instances[i].object_to_world[5] = instances[i].object_to_world[10] = 1.0f;
      instances[i].medium = i == 3 ? 0u : 1u;
    }
    instances[1].object_to_world[0] = -1.0f;
    scene.instances = instances.data(), scene.instance_count = 4;
    scene.meshes = meshes.data(), scene.mesh_count = 1;
    scene.material_slots = slots.data(), scene.material_headers = headers.data();
    scene.material_header_count = uint32_t(headers.size());
    scene.medium_count = 1;
    kg.scene = &scene;
  }
  Stage route(uint32_t inst, uint32_t prim, ar::Real3 d, uint32_t flags) const {
    return ar::route_hit(kg, ar::HitRecord{1.0f, prim, inst, 0.25f, 0.25f}, d, flags);
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("route_hit/medium routes to shade volume") {
  MediumRig r;
  // Triangle 0 faces +z: a ray going down meets its front, a ray going up its back. Triangle 1 is the
  // reverse. Only the back face of a medium-bearing instance (the segment ran inside it) goes to the
  // volume stage, for either path kind; a mirrored instance keeps its sides.
  for (const uint32_t inst : {0u, 1u}) {
    INFO("instance " << inst);
    CHECK(r.route(inst, 0, kDown, 0u) == Stage::SHADE_SURFACE);
    CHECK(r.route(inst, 0, kUp, 0u) == Stage::SHADE_VOLUME);
    CHECK(r.route(inst, 1, kDown, 0u) == Stage::SHADE_VOLUME);
    CHECK(r.route(inst, 1, kUp, 0u) == Stage::SHADE_SURFACE);
    CHECK(r.route(inst, 0, kUp, kLight) == Stage::SHADE_VOLUME);
    CHECK(r.route(inst, 0, kDown, kLight) == Stage::SHADE_LIGHT_PATH);
  }
  // The medium test comes before the extension test; the front face still reaches it.
  CHECK(r.route(2, 0, kUp, 0u) == Stage::SHADE_VOLUME);
  CHECK(r.route(2, 0, kDown, 0u) == Stage::SHADE_SURFACE_EXT);
  // An instance without a medium is never sent there, from either side.
  for (const uint32_t prim : {0u, 1u})
    for (const ar::Real3 d : {kDown, kUp}) CHECK(r.route(3, prim, d, 0u) == Stage::SHADE_SURFACE);
  // A miss is a miss.
  CHECK(ar::route_hit(r.kg, ar::HitRecord{5.0f, ~0u, ar::kNoInstance, 0, 0}, kUp, 0u) == Stage::SHADE_BACKGROUND);
}

TEST_CASE("route_hit/back face agrees with shade_context_geometry") {
  // hit_is_back_face repeats shade_context_geometry's side rule; the two must give the same side for
  // every triangle, direction and instance transform, a mirror included.
  MediumRig r;
  uint32_t checked = 0;
  for (uint32_t inst = 0; inst < 4; ++inst)
    for (uint32_t prim = 0; prim < 2; ++prim)
      for (const ar::Real3 d : {kDown, kUp, ar::Real3{0.3f, -0.2f, -1.0f}, ar::Real3{-0.3f, 0.2f, 1.0f}}) {
        ar::ShadeContext ctx{};
        REQUIRE(ar::shade_context_geometry(r.kg, inst, prim, ar::Real2{0.25f, 0.25f}, d, &ctx));
        const ar::HitRecord h{1.0f, prim, inst, 0.25f, 0.25f};
        CHECK(ar::hit_is_back_face(r.kg, h, d) == (ctx.front_face == 0u));
        ++checked;
      }
  CHECK(checked == 32u);
}

TEST_CASE("route_hit/camera path miss shades background") {
  SceneRig r;
  const ar::HitRecord miss{5.0f, ~0u, ar::kNoInstance, 0, 0};
  CHECK(ar::route_hit(r.kg, miss, kDown, 0u) == Stage::SHADE_BACKGROUND);
}

TEST_CASE("route_hit/light path miss terminates") {
  SceneRig r;
  const ar::HitRecord miss{5.0f, ~0u, ar::kNoInstance, 0, 0};
  CHECK(ar::route_hit(r.kg, miss, kDown, kLight) == Stage::TERMINATED);
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

TEST_CASE("route_hit/flags other than the path kind are ignored") {
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
