// SPDX-License-Identifier: Apache-2.0
// k_route_hit (route_hit as a CUDA kernel) against the CPU route_hit over every fixture case: each
// instance class x triangle x path-flag combination, a miss, with and without material headers.
// Ref: docs/decisions/0002-kernel-architecture.md D8 (k_route_hit), D11 (route_hit); 0005 item 17.
#include <doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "host/device.h"
#include "stages/route_hit.h"

using namespace astroray::host;
using ar::Stage;

namespace {

// Like the tables of tests/cpu/test_route_hit.cpp, with both triangles used: instances 0 plain,
// 1 lamp, 2 extension material, 3 no material slice, 4 non-opaque only, 5 lamp with an extension
// material; one mesh of two triangles whose material slots are 0 and 1. Instance i owns pool
// entries [2i, 2i + 2); each entry names a header: 0 Common, 1 EXTENSION, 2 NON_OPAQUE.
// Instances 6 and 7 bear a medium on a second mesh of two triangles facing +z and -z; 7 is mirrored
// in x (determinant -1), so the back-face test must agree with the CPU under a flip.
struct HostScene {
  std::vector<int32_t> tri_material{0, 1};
  std::vector<int32_t> tri_material_medium{0, 0};
  std::vector<uint32_t> tri_vertex{0, 1, 2, 0, 2, 1};
  std::vector<float> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::vector<ar::DeviceMeshView> meshes{2};
  std::vector<ar::DeviceInstanceRecord> instances{8};
  std::vector<uint32_t> slots{0, 1, 0, 0, 1, 0, 0, 0, 2, 0, 1, 1, 0, 0};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}, {0, 0, 0, ar::NON_OPAQUE}};
  HostScene() {
    meshes[0] = {};
    meshes[0].triangle_count = 2;
    for (uint32_t i = 0; i < 6; ++i) {
      instances[i] = {};
      instances[i].light_index = ar::kNoLightIndex;
      instances[i].material_offset = 2 * i, instances[i].material_count = i == 3 ? 0 : 2;
      instances[i].object_to_world[0] = instances[i].object_to_world[5] = instances[i].object_to_world[10] = 1.0f;
    }
    instances[1].light_index = 7;
    instances[5].light_index = 8;
    for (uint32_t i : {6u, 7u}) instances[i].mesh = 1, instances[i].medium = 1, instances[i].material_count = 1;
    instances[7].object_to_world[0] = -1.0f;
    meshes[1] = {};
    meshes[1].triangle_count = 2;
  }
};

template <class T>
DeviceBuffer put(Device& dev, LedgerCategory cat, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(cat, sizeof(T) * v.size(), Owner{1, 0});
  dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}

struct Case {
  uint32_t inst, prim, flags;
  float dir_z;  // the ray direction (0, 0, dir_z)
};

std::vector<Case> all_cases() {
  std::vector<Case> cases;
  const uint32_t flag_sets[] = {0u, ar::PATH_KIND, ar::FRONT_FACE | ar::PATH_KIND,
                                ar::FRONT_FACE | ar::LAST_BOUNCE_SPECULAR};
  for (uint32_t inst = 0; inst < 8; ++inst)
    for (uint32_t prim = 0; prim < 2; ++prim)
      for (uint32_t f : flag_sets)
        for (float dz : {-1.0f, 1.0f}) cases.push_back({inst, prim, f, dz});
  for (uint32_t f : flag_sets) cases.push_back({ar::kNoInstance, ~0u, f, -1.0f});
  return cases;
}

// Runs k_route_hit on the device over `cases` for a scene with or without material tables; returns
// the stage per case and the CPU route_hit's stage for the same case.
void run(bool with_materials, std::vector<uint8_t>& gpu, std::vector<uint8_t>& cpu) {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev = make_cuda_device(ledger, 0);
  const uint32_t module = dev->load_module_file(std::string(AR_TEST_KERNEL_DIR) + "/k_route_hit.cubin");
  const std::vector<Case> cases = all_cases();
  const uint32_t n = uint32_t(cases.size());

  HostScene hs;
  // CPU reference over host pointers.
  hs.meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(hs.tri_material.data()));
  hs.meshes[1].tri_material = uint64_t(reinterpret_cast<uintptr_t>(hs.tri_material_medium.data()));
  hs.meshes[1].tri_vertex = uint64_t(reinterpret_cast<uintptr_t>(hs.tri_vertex.data()));
  hs.meshes[1].positions = uint64_t(reinterpret_cast<uintptr_t>(hs.positions.data()));
  ar::SceneData cpu_scene{};
  cpu_scene.instances = hs.instances.data(), cpu_scene.instance_count = uint32_t(hs.instances.size());
  cpu_scene.meshes = hs.meshes.data(), cpu_scene.mesh_count = 2;
  cpu_scene.medium_count = 1;
  if (with_materials) {
    cpu_scene.material_slots = hs.slots.data(), cpu_scene.material_headers = hs.headers.data();
    cpu_scene.material_header_count = uint32_t(hs.headers.size());
  }
  ar::KernelGlobals cpu_kg{};
  cpu_kg.scene = &cpu_scene;
  cpu.clear();
  for (const Case& c : cases)
    cpu.push_back(uint8_t(ar::route_hit(cpu_kg, {1.0f, c.prim, c.inst, 0.25f, 0.25f}, ar::Real3{0, 0, c.dir_z}, c.flags)));

  // The same tables in device memory.
  DeviceBuffer tri_material = put(*dev, LedgerCategory::GEOMETRY, hs.tri_material);
  hs.meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material.ptr));
  DeviceBuffer tri_material_medium = put(*dev, LedgerCategory::GEOMETRY, hs.tri_material_medium);
  DeviceBuffer tri_vertex = put(*dev, LedgerCategory::GEOMETRY, hs.tri_vertex);
  DeviceBuffer positions = put(*dev, LedgerCategory::GEOMETRY, hs.positions);
  hs.meshes[1].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material_medium.ptr));
  hs.meshes[1].tri_vertex = uint64_t(reinterpret_cast<uintptr_t>(tri_vertex.ptr));
  hs.meshes[1].positions = uint64_t(reinterpret_cast<uintptr_t>(positions.ptr));
  DeviceBuffer meshes = put(*dev, LedgerCategory::TABLES, hs.meshes);
  DeviceBuffer instances = put(*dev, LedgerCategory::TABLES, hs.instances);
  DeviceBuffer slots = put(*dev, LedgerCategory::TABLES, hs.slots);
  DeviceBuffer headers = put(*dev, LedgerCategory::TABLES, hs.headers);
  ar::SceneData scene{};
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(instances.ptr), scene.instance_count = uint32_t(hs.instances.size());
  scene.meshes = static_cast<const ar::DeviceMeshView*>(meshes.ptr), scene.mesh_count = 2;
  scene.medium_count = 1;
  if (with_materials) {
    scene.material_slots = static_cast<const uint32_t*>(slots.ptr);
    scene.material_headers = static_cast<const ar::MaterialHeader*>(headers.ptr);
    scene.material_header_count = uint32_t(hs.headers.size());
  }
  DeviceBuffer scene_buf = put(*dev, LedgerCategory::TABLES, std::vector<ar::SceneData>{scene});

  std::vector<ar::Real> hit_t(n, 1.0f);
  std::vector<uint32_t> hit_prim(n), hit_inst(n), flags(n);
  std::vector<ar::Real2> hit_uv(n, ar::Real2{0.25f, 0.25f});
  std::vector<ar::Real3> ray_d(n);
  std::vector<uint8_t> next(n, 0xEE);
  for (uint32_t i = 0; i < n; ++i) hit_prim[i] = cases[i].prim, hit_inst[i] = cases[i].inst, flags[i] = cases[i].flags, ray_d[i] = ar::Real3{0, 0, cases[i].dir_z};
  DeviceBuffer b_t = put(*dev, LedgerCategory::MAIN_PATH_POOL, hit_t), b_prim = put(*dev, LedgerCategory::MAIN_PATH_POOL, hit_prim),
               b_inst = put(*dev, LedgerCategory::MAIN_PATH_POOL, hit_inst), b_flags = put(*dev, LedgerCategory::MAIN_PATH_POOL, flags),
               b_uv = put(*dev, LedgerCategory::MAIN_PATH_POOL, hit_uv), b_ray = put(*dev, LedgerCategory::MAIN_PATH_POOL, ray_d), b_next = put(*dev, LedgerCategory::MAIN_PATH_POOL, next);
  ar::PathStateSoA path{};
  path.hit_t = static_cast<ar::Real*>(b_t.ptr), path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), path.flags = static_cast<uint32_t*>(b_flags.ptr);
  path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), path.ray_d = static_cast<ar::Real3*>(b_ray.ptr), path.next_stage = static_cast<uint8_t*>(b_next.ptr);
  DeviceBuffer path_buf = put(*dev, LedgerCategory::TABLES, std::vector<ar::PathStateSoA>{path});
  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr), kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  DeviceBuffer kg_buf = put(*dev, LedgerCategory::TABLES, std::vector<ar::KernelGlobals>{kg});

  const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
  uint32_t count = n;
  void* args[] = {&kg_dev, &count};
  // Fewer threads than items: the grid-stride loop must cover the tail.
  dev->launch_module_kernel(module, "k_route_hit", 1, 16, args);
  dev->synchronize();
  gpu.assign(n, 0);
  dev->download(gpu.data(), b_next, 0, n);

  for (DeviceBuffer* b : {&tri_material, &meshes, &instances, &slots, &headers, &scene_buf, &b_t, &b_prim, &b_inst, &b_flags,
                          &b_uv, &b_ray, &b_next, &path_buf, &kg_buf, &tri_material_medium, &tri_vertex, &positions})
    dev->free(*b);
  CHECK(ledger.total() == 0);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("route_hit_gpu/matches cpu") {
  for (const bool with_materials : {true, false}) {
    INFO("with material tables: " << with_materials);
    std::vector<uint8_t> gpu, cpu;
    run(with_materials, gpu, cpu);
    REQUIRE(gpu.size() == cpu.size());
    const std::vector<Case> cases = all_cases();
    uint32_t differing = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
      if (gpu[i] != cpu[i]) {
        ++differing;
        MESSAGE("case inst " << cases[i].inst << " prim " << cases[i].prim << " flags " << cases[i].flags << ": gpu "
                             << int(gpu[i]) << " cpu " << int(cpu[i]));
      }
    }
    CHECK(differing == 0);
    // The fixture really exercises every route: more than one distinct stage came back.
    uint32_t seen[ar::kStageCount] = {};
    for (uint8_t s : cpu) ++seen[s];
    uint32_t distinct = 0;
    for (uint32_t c : seen) distinct += c != 0;
    CHECK(distinct >= (with_materials ? 8u : 6u));
    CHECK(seen[uint32_t(Stage::SHADE_VOLUME)] > 0u);  // the medium instances route by side
  }
}

}  // TEST_SUITE
