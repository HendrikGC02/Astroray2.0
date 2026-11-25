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
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

// ---- the stage in isolation ----

// Four medium-bearing instances of a one-triangle mesh: 0 plain, 1 extension material, 2 lamp, and
// item 3 is a light path on instance 0. The medium is homogeneous with CONSTANT coefficients (the
// program {reserved, VOLUME_COEFFS, END}); vacuum by default. Every camera item's segment runs from
// the origin along +z for hit_t = 1.
struct StageRig {
  std::vector<int32_t> tri_material{0};
  std::vector<ar::DeviceMeshView> meshes{1};
  std::vector<ar::DeviceInstanceRecord> instances{3};
  std::vector<uint32_t> slots{0, 1};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}};
  std::vector<uint32_t> program{0u, uint32_t(ar::Op::VOLUME_COEFFS), uint32_t(ar::Op::END)};
  ar::MediumBlock block = ar::medium_block_vacuum();
  ar::MediumDesc desc{};
  ar::SceneData scene{};
  ar::SceneTables tables{};
  ar::FilmData film{};
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  static constexpr uint32_t kN = 4;
  std::vector<uint32_t> queue{9, 9, 9, 9, 0, 1, 2, 3}, hit_prim{0, 0, 0, 0}, hit_inst{0, 1, 2, 0}, flags{0, 0, 0, ar::PATH_KIND};
  std::vector<uint32_t> sample_id{0, 1, 2, 3}, active{0, 1, 2, 3}, prev_ng{0, 0, 0, 0}, prev_ns{0, 0, 0, 0};
  std::vector<uint32_t> medium_rng{0, 0, 0, 0}, slot_flags{0, 0, 0, 0};
  std::vector<float> slot_L = std::vector<float>(4 * kN, 0.0f);
  std::vector<ar::Real> hit_t{1, 1, 1, 1}, prev_dist{0, 0, 0, 0};
  std::vector<ar::Real2> hit_uv{{0.25f, 0.25f}, {0.25f, 0.25f}, {0.25f, 0.25f}, {0.25f, 0.25f}};
  std::vector<ar::Real3> ray_o = std::vector<ar::Real3>(kN, ar::Real3{0, 0, 0});
  std::vector<ar::Real3> ray_d = std::vector<ar::Real3>(kN, ar::Real3{0, 0, 1});
  std::vector<ar::Spec> lambda_t = std::vector<ar::Spec>(kN, ar::Spec{{450, 520, 590, 660}});
  std::vector<ar::Spec> beta = std::vector<ar::Spec>(kN, ar::spec_fill(1)), r_u = beta, r_l = beta;
  std::vector<ar::U16x6> bounces = std::vector<ar::U16x6>(kN, ar::U16x6{});
  std::vector<uint16_t> event_index{3, 3, 3, 3};
  std::vector<uint8_t> next{0xEE, 0xEE, 0xEE, 0xEE}, term{0, 0, 0, 0}, ray_type{1, 1, 1, 1};
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
    block.entry = 1u;
    scene.instances = instances.data(), scene.instance_count = 3;
    scene.meshes = meshes.data(), scene.mesh_count = 1;
    scene.material_slots = slots.data(), scene.material_headers = headers.data();
    scene.material_header_count = uint32_t(headers.size());
    scene.medium_descs = &desc, scene.medium_count = 1;
    scene.medium_blocks = &block, scene.medium_programs = program.data(), scene.medium_program_words = 3;
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data(), path.flags = flags.data(), path.next_stage = next.data();
    path.sample_id = sample_id.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.bounces = bounces.data(), path.event_index = event_index.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data(), path.medium_rng = medium_rng.data();
    film.slot_capacity = kN, film.slot_flags = slot_flags.data(), film.slot_L = slot_L.data();
    film.bounce_sum = film.bounce_max = film.bounce_paths = film.term_hist = -1;
    kg.scene = &scene, kg.path = &path, kg.main_queue = queue.data(), kg.tables = &tables, kg.film = &film;
    kg.active_pixels = active.data();
    kg.session.samples_per_pass = 1, kg.session.seed = 11, kg.session.sampler_mode = ar::SAMPLER_SOBOL;
    kg.session.features.media = true;
    for (uint32_t& m : kg.session.max_bounces) m = 8;
  }
  // A homogeneous medium of grey CONSTANT coefficients, one Henyey-Greenstein lobe.
  void medium(float sigma_a, float sigma_s, float g) {
    block.sigma_a = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, ar::Real(sigma_a));
    block.sigma_s[0] = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, ar::Real(sigma_s));
    block.g[0] = g;
    block.lobe_count = 1;
  }
  void run() {
    const ar::StepEntry step{4, kN, 0};
    for (uint32_t item = 0; item < kN; ++item) ar::stage_shade_volume(kg, item, step);
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_volume/forwards to the stage route_hit gives without the medium test") {
  StageRig r;  // vacuum: every camera item reaches the hit with its weights unchanged
  const ar::StepEntry step{4, StageRig::kN, 0};
  r.run();
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

TEST_CASE("shade_volume/scatter updates the path state") {
  // sigma_s = 1e4 over a segment of 1: a real scatter at about 1e-4 is certain in float.
  StageRig r;
  r.medium(0.0f, 1.0e4f, 0.5f);
  r.flags[0] |= ar::LAST_BOUNCE_SPECULAR;
  r.run();
  CHECK(r.next[0] == uint8_t(Stage::INTERSECT_CLOSEST));
  CHECK(r.term[0] == 0u);
  for (int i = 0; i < ar::kLanes; ++i) {
    CHECK(r.beta[0].v[i] == ar::Real(1));  // grey: T sigma_s / (T_h sigma_s_h) = 1, and HG's p / pdf = 1
    CHECK(r.r_u[0].v[i] == ar::Real(1));
    CHECK(r.r_l[0].v[i] == ar::Real(0));  // no light strategy at the vertex: emission weighs 1 / avg(r_u)
  }
  CHECK((r.flags[0] & ar::LAST_BOUNCE_SPECULAR) == 0u);
  CHECK(r.bounces[0].v[0] == 1u);
  CHECK(r.bounces[0].v[4] == 1u);
  CHECK(r.event_index[0] == 4u);
  CHECK(r.ray_type[0] == uint8_t(AR_VISIBILITY_VOLUME));
  CHECK(ar::kRayTypeVolume == uint8_t(AR_VISIBILITY_VOLUME));
  CHECK(r.ray_o[0].z > ar::Real(0));
  CHECK(r.ray_o[0].z < ar::Real(0.01));
  CHECK(std::fabs(double(ar::length(r.ray_d[0])) - 1.0) < 1e-5);
  CHECK(r.prev_dist[0] == ar::Real(0));
  CHECK(r.medium_rng[0] == 3u * ar::kFlightCollisionLimit + 1u);  // e = 3, one tentative collision
  // The light path passes through untouched.
  CHECK(r.next[3] == uint8_t(Stage::SHADE_LIGHT_PATH));
  CHECK(r.beta[3].v[0] == ar::Real(1));
}

TEST_CASE("shade_volume/absorption and bounce limits end the path") {
  StageRig a;
  a.medium(1.0e4f, 0.0f, 0.0f);
  a.run();
  CHECK(a.next[0] == uint8_t(Stage::TERMINATED));
  CHECK(a.term[0] == uint8_t(ar::Term::ABSORBED));
  // A scatter with volume_bounces used up ends the path (terminate-after, 0006 D14), as does the total.
  StageRig v;
  v.medium(0.0f, 1.0e4f, 0.0f);
  v.kg.session.max_bounces[4] = 2;
  v.bounces[0].v[4] = 2, v.bounces[1].v[4] = 1;
  v.kg.session.max_bounces[0] = 5;
  v.bounces[2].v[0] = 5;
  v.run();
  CHECK(v.term[0] == uint8_t(ar::Term::MAX_BOUNCE));
  CHECK(v.next[1] == uint8_t(Stage::INTERSECT_CLOSEST));  // scatter 2 of at most 2 continues
  CHECK(v.term[2] == uint8_t(ar::Term::MAX_BOUNCE));
}

TEST_CASE("shade_volume/stale medium_rng gives the same result") {
  // A pool slot reused from another path carries that path's counter; each flight starts its own at
  // e * 2^20, so the result does not depend on it. Run twice, the second time from stale counters.
  const auto run = [](uint32_t stale) {
    auto r = std::make_unique<StageRig>();
    r->medium(0.5f, 1.0f, 0.3f);
    for (uint32_t i = 0; i < StageRig::kN; ++i) r->hit_t[i] = ar::Real(1.5), r->medium_rng[i] = stale + 977u * i;
    r->event_index = {1, 2, 3, 4};
    r->run();
    return r;
  };
  const auto a = run(0u), b = run(0xDEADBEEFu);
  for (uint32_t i = 0; i < StageRig::kN; ++i) {
    INFO("item " << i);
    CHECK(a->next[i] == b->next[i]);
    CHECK(a->term[i] == b->term[i]);
    CHECK(std::memcmp(&a->beta[i], &b->beta[i], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&a->r_u[i], &b->r_u[i], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&a->ray_o[i], &b->ray_o[i], sizeof(ar::Real3)) == 0);
    CHECK(std::memcmp(&a->ray_d[i], &b->ray_d[i], sizeof(ar::Real3)) == 0);
    if (i < 3u) CHECK(a->medium_rng[i] == b->medium_rng[i]);
  }
  CHECK(a->medium_rng[0] > 1u * ar::kFlightCollisionLimit);
}

TEST_CASE("shade_volume/zero sigma bit identical") {
  const auto render = [](bool with_medium) {
    slab::SlabRig rig(SceneConfig{2, {}, {}}, with_medium);
    std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
    return slab::render_slab(rig, *dev);
  };
  const slab::Render with = render(true), without = render(false);
  slab::check_identity(with, without);
  // The session turns on the MEDIA path-state fields exactly when the scene has a medium: the
  // scheduler counts path_state_bytes of its feature set per main-pool item.
  ar::StateFeatures on{}, off{};
  on.media = true;
  REQUIRE(ar::path_state_bytes(on) != ar::path_state_bytes(off));
  REQUIRE(with.main_items > 0u);
  REQUIRE(without.main_items > 0u);
  CHECK(with.state_bytes == with.main_items * ar::path_state_bytes(on));
  CHECK(without.state_bytes == without.main_items * ar::path_state_bytes(off));
}

}  // TEST_SUITE

// The tracking cases (0006 V1) on the CPU device; their CUDA twins are tests/gpu/test_volume_agree.cpp,
// which the gate runs in the same concurrent group.
TEST_SUITE("gate") {

TEST_CASE("shade_volume/chromatic slab transmittance") {
  slab::SlabRig rig(SceneConfig{2, {}, {}}, slab::chromatic_spec());
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 4);
  const slab::Render r = slab::render_slab(rig, *dev);
  slab::check_chromatic(rig, r, "shade_volume/chromatic slab transmittance");
}

TEST_CASE("shade_volume/scattering slab") {
  slab::SlabRig rig(SceneConfig{2, {}, {}}, slab::scattering_spec());
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 4);
  const slab::Render r = slab::render_slab(rig, *dev);
  slab::check_scattering(r, "shade_volume/scattering slab");
}

}  // TEST_SUITE
