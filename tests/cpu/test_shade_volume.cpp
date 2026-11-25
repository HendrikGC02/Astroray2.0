// SPDX-License-Identifier: Apache-2.0
// shade_volume skeleton: the stage forwards every item to the stage route_hit would have chosen
// without the medium test, and a slab scene whose medium has sigma = 0 renders bit for bit like the
// same scene without a medium while the stage launches exactly once per medium segment.
// The render cases use common/slab_rig.h (0006 V1's shape). Identity is claimed for that scene only: each
// sample adds one contribution, so float addition order cannot differ; a scene with next-event estimation
// or emission at neighbouring vertices does not have that guarantee across a medium segment.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11; 0006 D8 (flow), V1; 0004 D3.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/slab_rig.h"
#include "device/cpu/cpu_device.h"
#include "stages/shade_volume.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

// ---- the stage in isolation ----

// Four medium-bearing instances of a one-triangle mesh: 0 plain, 1 extension material, 2 lamp, and
// item 3 is a light path on instance 0.
struct StageRig {
  std::vector<int32_t> tri_material{0};
  std::vector<ar::DeviceMeshView> meshes{1};
  std::vector<ar::DeviceInstanceRecord> instances{3};
  std::vector<uint32_t> slots{0, 1};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}};
  ar::SceneData scene{};
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  static constexpr uint32_t kN = 4;
  std::vector<uint32_t> queue{9, 9, 9, 9, 0, 1, 2, 3}, hit_prim{0, 0, 0, 0}, hit_inst{0, 1, 2, 0}, flags{0, 0, 0, ar::PATH_KIND};
  std::vector<ar::Real> hit_t{1, 1, 1, 1};
  std::vector<ar::Real2> hit_uv{{0.25f, 0.25f}, {0.25f, 0.25f}, {0.25f, 0.25f}, {0.25f, 0.25f}};
  std::vector<uint8_t> next{0xEE, 0xEE, 0xEE, 0xEE};
  StageRig() {
    meshes[0] = {};
    meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material.data()));
    meshes[0].triangle_count = 1;
    for (uint32_t i = 0; i < 3; ++i) {
      instances[i] = {};
      instances[i].light_index = ar::kNoLightIndex;
      instances[i].medium = 1;
      instances[i].material_offset = i == 1 ? 1u : 0u, instances[i].material_count = 1;
    }
    instances[2].light_index = 5;
    scene.instances = instances.data(), scene.instance_count = 3;
    scene.meshes = meshes.data(), scene.mesh_count = 1;
    scene.material_slots = slots.data(), scene.material_headers = headers.data();
    scene.material_header_count = uint32_t(headers.size());
    scene.medium_count = 1;
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data(), path.flags = flags.data(), path.next_stage = next.data();
    kg.scene = &scene, kg.path = &path, kg.main_queue = queue.data();
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_volume/forwards to the stage route_hit gives without the medium test") {
  StageRig r;
  const ar::StepEntry step{4, StageRig::kN, 0};
  for (uint32_t item = 0; item < StageRig::kN; ++item) ar::stage_shade_volume(r.kg, item, step);
  CHECK(r.next[0] == uint8_t(Stage::SHADE_SURFACE));
  CHECK(r.next[1] == uint8_t(Stage::SHADE_SURFACE_EXT));
  CHECK(r.next[2] == uint8_t(Stage::SHADE_LIGHT));  // a lamp behind a medium is not shaded as a surface
  CHECK(r.next[3] == uint8_t(Stage::SHADE_LIGHT_PATH));
  // The CPU chunk function does the same over a range of items.
  std::fill(r.next.begin(), r.next.end(), uint8_t(0xEE));
  cpu_stage_shade_volume(r.kg, step, 0, StageRig::kN);
  CHECK(r.next[0] == uint8_t(Stage::SHADE_SURFACE));
  CHECK(r.next[2] == uint8_t(Stage::SHADE_LIGHT));
}

TEST_CASE("shade_volume/zero sigma bit identical") {
  const auto render = [](bool with_medium) {
    slab::SlabRig rig(SceneConfig{2, {}, {}}, with_medium);
    std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
    return slab::render_slab(rig, *dev);
  };
  slab::check_identity(render(true), render(false));
}

}  // TEST_SUITE
