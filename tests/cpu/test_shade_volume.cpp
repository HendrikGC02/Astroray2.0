// SPDX-License-Identifier: Apache-2.0
// shade_volume skeleton: the stage forwards every item to the stage route_hit would have chosen
// without the medium test, and a slab scene whose medium has sigma = 0 renders bit for bit like the
// same scene without a medium while the stage launches exactly once per medium segment, and a flight
// requeued at 0006 D8's per-launch cap goes on bit-identically and lets its pass end.
// The render cases use common/slab_rig.h (0006 V1's shape). Identity is claimed for that scene only: each
// sample adds one contribution, so float addition order cannot differ; a scene with next-event estimation
// or emission at neighbouring vertices does not have that guarantee across a medium segment.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11; 0006 D8 (flow), V1; 0004 D3.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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
// item 3 is a light path on instance 0. Every path's stack holds that medium, the one the stage tracks.
// The medium is homogeneous with CONSTANT coefficients (the
// program {reserved, VOLUME_COEFFS, END}); vacuum by default. Every camera item's segment runs from
// the origin along +z for hit_t = 1.
constexpr uint32_t kDepth1 = 1u << ar::MEDIUM_DEPTH_SHIFT;

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
  std::vector<uint32_t> queue{9, 9, 9, 9, 0, 1, 2, 3}, hit_prim{0, 0, 0, 0}, hit_inst{0, 1, 2, 0};
  // Every path is inside medium 0: one stack entry (id 1 = 1 + the medium index, priority 0), depth 1.
  std::vector<uint32_t> flags{kDepth1, kDepth1, kDepth1, ar::PATH_KIND | kDepth1};
  std::vector<ar::U32x4> medium_stack = std::vector<ar::U32x4>(kN, ar::U32x4{{1u << 8, 0, 0, 0}});
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
    path.prev_dist = prev_dist.data(), path.medium_rng = medium_rng.data(), path.medium_stack = medium_stack.data();
    film.slot_capacity = kN, film.slot_flags = slot_flags.data(), film.slot_L = slot_L.data();
    film.bounce_sum = film.bounce_max = film.bounce_paths = film.term_hist = -1;
    film.guide_albedo = film.guide_normal = -1;  // no denoiser guides unless a case asks
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

TEST_CASE("shade_volume/the medium is the stack's highest entry") {
  // Two media: 0 vacuum, 1 scattering (sigma_s = 1e4, a scatter is certain). Every instance bears medium 0;
  // the stage tracks the stack's highest entry (greatest priority), not the hit instance's medium and
  // not the latest entry: the highest is entered first here.
  for (const bool scattering_on_top : {true, false}) {
    INFO("scattering medium on top: " << scattering_on_top);
    StageRig r;
    r.medium(0.0f, 1.0e4f, 0.5f);
    ar::MediumBlock vacuum = ar::medium_block_vacuum();
    vacuum.entry = 1u;
    std::vector<ar::MediumBlock> blocks{vacuum, r.block};
    std::vector<ar::MediumDesc> descs(2, r.desc);
    descs[0].program = 0, descs[1].program = 1;
    r.scene.medium_descs = descs.data(), r.scene.medium_blocks = blocks.data(), r.scene.medium_count = 2;
    const uint32_t lo = scattering_on_top ? 0u : 1u, hi = 1u - lo;  // medium indices at priority 0 and 3
    for (uint32_t s = 0; s < StageRig::kN; ++s) {
      r.medium_stack[s] = ar::U32x4{{ar::stack_entry(hi + 1u, 3u), ar::stack_entry(lo + 1u, 0u), 0, 0}};
      r.flags[s] = ar::set_medium_depth(r.flags[s], 2u);
    }
    r.run();
    CHECK(r.next[0] == uint8_t(scattering_on_top ? Stage::INTERSECT_CLOSEST : Stage::SHADE_SURFACE));
    CHECK(r.bounces[0].v[4] == (scattering_on_top ? 1u : 0u));
  }
}

TEST_CASE("shade_volume/flights across a false interface draw new blocks") {
  // 2^14 paths in a grey absorbing medium (sigma_a = 1 per m) run two flights at one event index, of
  // L1 = 0.5 m and L2 = 0.7 m, as a false interface (or a lamp crossing) between them leaves the event
  // index alone. The second flight continues the medium stream, so the fraction that reaches both
  // ends is exp(-sigma (L1 + L2)) within 3 standard errors (fixed seed); flights that reused the first
  // flight's blocks would give exp(-sigma max(L1, L2)).
  constexpr uint32_t n = 1u << 14;
  constexpr double sigma = 1.0, l1 = 0.5, l2 = 0.7;
  StageRig r;
  r.medium(float(sigma), 0.0f, 0.0f);
  std::vector<uint32_t> queue(n), active(n), sample_id(n), flags(n, kDepth1), hit_prim(n, 0u), hit_inst(n, 0u), zero(n, 0u);
  std::vector<uint32_t> medium_rng(n, 0u), slot_flags(n, 0u);
  std::vector<ar::U32x4> stack(n, ar::U32x4{{1u << 8, 0, 0, 0}});
  std::vector<float> slot_L(4 * size_t(n), 0.0f);
  std::vector<ar::Real> hit_t(n, ar::Real(l1)), prev_dist(n, ar::Real(0));
  std::vector<ar::Real2> hit_uv(n, ar::Real2{0.25f, 0.25f});
  std::vector<ar::Real3> ray_o(n, ar::Real3{0, 0, 0}), ray_d(n, ar::Real3{0, 0, 1});
  std::vector<ar::Spec> lambda_t(n, ar::Spec{{450, 520, 590, 660}}), beta(n, ar::spec_fill(1)), r_u = beta, r_l = beta;
  std::vector<ar::U16x6> bounces(n, ar::U16x6{});
  std::vector<uint16_t> event_index(n, 3);
  std::vector<uint8_t> next(n, uint8_t(Stage::SHADE_VOLUME)), term(n, 0), ray_type(n, 1);
  for (uint32_t i = 0; i < n; ++i) queue[i] = active[i] = sample_id[i] = i;
  ar::PathStateSoA& p = r.path;
  p.hit_t = hit_t.data(), p.hit_prim = hit_prim.data(), p.hit_inst = hit_inst.data(), p.hit_uv = hit_uv.data();
  p.flags = flags.data(), p.next_stage = next.data(), p.sample_id = sample_id.data(), p.ray_o = ray_o.data();
  p.ray_d = ray_d.data(), p.lambda_t = lambda_t.data(), p.beta = beta.data(), p.r_u = r_u.data(), p.r_l = r_l.data();
  p.bounces = bounces.data(), p.event_index = event_index.data(), p.term = term.data(), p.ray_type = ray_type.data();
  p.prev_ng = zero.data(), p.prev_ns = zero.data(), p.prev_dist = prev_dist.data(), p.medium_rng = medium_rng.data();
  p.medium_stack = stack.data();
  r.film.slot_capacity = n, r.film.slot_flags = slot_flags.data(), r.film.slot_L = slot_L.data();
  r.kg.main_queue = queue.data(), r.kg.active_pixels = active.data();
  r.kg.session.sampler_mode = ar::SAMPLER_INDEPENDENT;
  cpu_stage_shade_volume(r.kg, ar::StepEntry{0, n, 0}, 0, n);
  // The survivors' second flight from the first one's end; the absorbed are done.
  std::vector<uint32_t> second;
  for (uint32_t i = 0; i < n; ++i) {
    if (next[i] != uint8_t(Stage::SHADE_SURFACE)) continue;
    second.push_back(i);
    next[i] = uint8_t(Stage::SHADE_VOLUME), ray_o[i] = ar::Real3{0, 0, ar::Real(l1)}, hit_t[i] = ar::Real(l2);
  }
  r.kg.main_queue = second.data();
  cpu_stage_shade_volume(r.kg, ar::StepEntry{0, uint32_t(second.size()), 0}, 0, uint32_t(second.size()));
  uint32_t reached = 0;
  for (uint32_t i : second) reached += next[i] == uint8_t(Stage::SHADE_SURFACE);
  const double mean = double(reached) / n, se = std::sqrt(mean * (1.0 - mean) / n), truth = std::exp(-sigma * (l1 + l2));
  std::printf("shade_volume two flights: reached %.5f truth %.5f se %.2e z %+.2f (shared blocks would give %.5f)\n", mean, truth,
              se, (mean - truth) / se, std::exp(-sigma * std::max(l1, l2)));
  CHECK(std::fabs(mean - truth) < 3.0 * se);
}

TEST_CASE("shade_volume/a volume limit end is counted once") {
  // Each camera item has one collision left in its event's block range (medium_rng one below the range's
  // end), and the majorant is 4096 sigma_t, so its first tentative collision is null with probability
  // 4095/4096 and uses the range up: the flight ends VOLUME_LIMIT. Each such end adds one to
  // FilmData::medium_limit_total, with no statistics plane requested; the light path (item 3) is
  // forwarded and counts nothing. A rig whose film has no counter (null) runs the same ends.
  const auto rig = [](uint32_t* counter) {
    auto r = std::make_unique<StageRig>();
    r->medium(0.0f, 1.0f, 0.0f);
    r->kg.session.medium_majorant = 4096u;
    r->film.medium_limit_total = counter;
    for (uint32_t i = 0; i < StageRig::kN; ++i) r->medium_rng[i] = ar::flight_rng_start(4u) - 1u;
    return r;
  };
  uint32_t total = 0;
  auto r = rig(&total);
  r->run();
  for (uint32_t i = 0; i < 3u; ++i) {
    INFO("item " << i);
    CHECK(r->term[i] == uint8_t(ar::Term::VOLUME_LIMIT));
    CHECK(r->next[i] == uint8_t(Stage::TERMINATED));
  }
  CHECK(r->next[3] != uint8_t(Stage::TERMINATED));
  CHECK(total == 3u);
  auto none = rig(nullptr);
  none->run();
  CHECK(none->term[0] == uint8_t(ar::Term::VOLUME_LIMIT));
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

TEST_CASE("shade_volume/a scatter before the denoising surface ends the guides") {
  // Pass-through fog (0008 d15, PROVISIONAL): with guide planes, a real scatter sets
  // DENOISE_SURFACE_DONE, writes nothing (both planes stay 0 and finite) and leaves the tint; a path
  // that crosses the medium without scattering keeps the bit clear for its surface.
  StageRig r;
  r.medium(0.0f, 1.0e4f, 0.5f);
  std::vector<float> add(6u * StageRig::kN, 0.0f);
  std::vector<ar::Real3> tint(StageRig::kN, ar::Real3{1, 1, 1});
  r.film.add = add.data(), r.film.plane_stride = StageRig::kN, r.film.guide_albedo = 0, r.film.guide_normal = 3;
  r.path.guide_tint = tint.data();
  r.kg.session.features.guides = true;
  r.run();
  REQUIRE(r.next[0] == uint8_t(Stage::INTERSECT_CLOSEST));
  CHECK((r.flags[0] & ar::DENOISE_SURFACE_DONE) != 0u);
  for (float v : add) CHECK(v == 0.0f);
  CHECK(tint[0].x == ar::Real(1));
  StageRig vac;  // vacuum: the segment reaches its surface
  vac.film.add = add.data(), vac.film.plane_stride = StageRig::kN, vac.film.guide_albedo = 0, vac.film.guide_normal = 3;
  vac.path.guide_tint = tint.data();
  vac.kg.session.features.guides = true;
  vac.run();
  CHECK(vac.next[0] == uint8_t(Stage::SHADE_SURFACE));
  CHECK((vac.flags[0] & ar::DENOISE_SURFACE_DONE) == 0u);
}

TEST_CASE("shade_volume/unsupported media end INVALID") {
  // Each medium the stage cannot render fails the sample (its slot holds NaN) and ends the path
  // INVALID: an emitting medium, a second phase lobe, a volume entry that stops with an error, and a
  // NaN or infinite coefficient (which tracking would read as a zero majorant). The light path
  // (item 3) is forwarded either way.
  const auto run = [](const std::function<void(StageRig&)>& setup) {
    auto r = std::make_unique<StageRig>();
    r->medium(0.5f, 1.0f, 0.3f);
    setup(*r);
    r->run();
    return r;
  };
  const auto invalid = [](const StageRig& r, const char* what) {
    INFO(what);
    for (uint32_t i = 0; i < 3u; ++i) {
      CHECK(r.term[i] == uint8_t(ar::Term::INVALID));
      CHECK(r.next[i] == uint8_t(Stage::TERMINATED));
      CHECK(std::isnan(r.slot_L[4 * i]));
    }
    CHECK(r.next[3] == uint8_t(Stage::SHADE_LIGHT_PATH));
  };
  invalid(*run([](StageRig& r) { r.desc.flags = ar::MEDIUM_HAS_EMISSION; }), "emission");
  invalid(*run([](StageRig& r) {
            r.block.sigma_s[1] = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, ar::Real(0.25));
            r.block.lobe_count = 2;
          }),
          "second lobe");
  // A CONSTANT REFLECTANCE ref is outside medium_ref_ok: the volume entry stops with BAD_OPERAND.
  invalid(*run([](StageRig& r) { r.block.sigma_a = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(0.5)); }),
          "volume entry error");
  invalid(*run([](StageRig& r) {
            r.block.sigma_a = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, ar::Real(std::nan("")));
          }),
          "NaN sigma_a");
  invalid(*run([](StageRig& r) {
            r.block.sigma_s[0] = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, ar::Real(INFINITY));
          }),
          "infinite sigma_s");
  // The same rig with the plain medium is valid: the checks above are what fails it.
  const auto ok = run([](StageRig&) {});
  CHECK(ok->term[0] != uint8_t(ar::Term::INVALID));
}

TEST_CASE("shade_volume/stale medium_rng gives the same result") {
  // A pool slot reused from another path carries that path's counter; one outside the event's block
  // range is not read (the flight starts at e * 2^20), so the result does not depend on it. Run twice,
  // the second time from stale counters, both outside the range.
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

TEST_CASE("shade_volume/requeue continues the flight") {
  // 0006 D8 requeue at the stage: a grey scattering medium under a majorant 16 sigma_t (the gate hook's
  // value, RULINGS 111), so flights take null collisions; ray_d of length 2 and prev_dist 0.25, so the
  // parameter-unit state is exercised. Run once at the default cap and once at cap 1, relaunching every
  // requeued item (next_stage SHADE_VOLUME) until none is left: every path field the stage writes is
  // bit-identical, and medium_rng advanced by exactly one block per launch at cap 1.
  const auto rig = [](uint32_t cap) {
    auto r = std::make_unique<StageRig>();
    r->medium(0.0f, 0.5f, 0.3f);  // no absorption: every flight scatters or reaches, so its state lives on
    r->kg.session.medium_majorant = 16u;
    r->kg.session.medium_launch_cap = cap;
    for (uint32_t i = 0; i < StageRig::kN; ++i) {
      r->hit_t[i] = ar::Real(0.75), r->ray_d[i] = ar::Real3{0, 0, 2}, r->prev_dist[i] = ar::Real(0.25);
      r->medium_rng[i] = ar::flight_rng_start(3u);
    }
    return r;
  };
  const auto relaunch = [](StageRig& r, std::vector<uint32_t>* launches) {
    std::vector<uint32_t> items{0, 1, 2};  // item 3 is a light path, forwarded untouched
    while (!items.empty()) {
      for (uint32_t i : items) ++(*launches)[i];
      r.kg.main_queue = items.data();
      cpu_stage_shade_volume(r.kg, ar::StepEntry{0, uint32_t(items.size()), 0}, 0, uint32_t(items.size()));
      std::vector<uint32_t> again;
      for (uint32_t i : items)
        if (r.next[i] == uint8_t(Stage::SHADE_VOLUME)) again.push_back(i);
      items = again;
    }
  };
  auto ref = rig(ar::kMediumLaunchCap), cap1 = rig(1u);
  std::vector<uint32_t> n_ref(StageRig::kN, 0u), n_cap(StageRig::kN, 0u);
  relaunch(*ref, &n_ref);
  relaunch(*cap1, &n_cap);
  uint32_t requeues = 0, live = 0;
  for (uint32_t i = 0; i < 3u; ++i) {
    INFO("item " << i);
    CHECK(n_ref[i] == 1u);
    requeues += n_cap[i] - 1u;
    CHECK(cap1->medium_rng[i] - ar::flight_rng_start(3u) == n_cap[i]);
    CHECK(ref->medium_rng[i] == cap1->medium_rng[i]);
    CHECK(ref->next[i] == cap1->next[i]);
    CHECK(ref->term[i] == cap1->term[i]);
    // An ended path's fields are dead, and so is hit_t after a scatter (the next intersection writes it).
    if (ref->term[i] != 0u) continue;
    const bool scattered = ref->next[i] == uint8_t(Stage::INTERSECT_CLOSEST);
    ++live;
    CHECK(std::memcmp(&ref->beta[i], &cap1->beta[i], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&ref->r_u[i], &cap1->r_u[i], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&ref->r_l[i], &cap1->r_l[i], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&ref->ray_o[i], &cap1->ray_o[i], sizeof(ar::Real3)) == 0);
    CHECK(std::memcmp(&ref->ray_d[i], &cap1->ray_d[i], sizeof(ar::Real3)) == 0);
    if (!scattered) CHECK(std::memcmp(&ref->hit_t[i], &cap1->hit_t[i], sizeof(ar::Real)) == 0);
    CHECK(std::memcmp(&ref->prev_dist[i], &cap1->prev_dist[i], sizeof(ar::Real)) == 0);
    CHECK(ref->event_index[i] == cap1->event_index[i]);
  }
  std::printf("shade_volume requeue at the stage: %u requeues over 3 flights, %u still live\n", requeues, live);
  CHECK(requeues > 3u);
  CHECK(live > 0u);  // at least one flight goes on, so its state was compared
}

TEST_CASE("shade_volume/reached write-back keeps the hit and the previous vertex") {
  // A flight that reaches its hit after null collisions has rebased its ray, so the stage writes the
  // moved origin, the shortened hit_t and the grown prev_dist back. shade_light reads o + hit_t d (the
  // lamp crossing) and shade_common o - prev_dist d (the previous vertex), so both must still be the
  // points they were before the flight: checked here to 32 ulp of the coordinates' scale, under a
  // majorant 64 sigma_t (null collisions in most flights), |ray_d| = 2 and prev_dist 0.25.
  StageRig r;
  r.medium(0.0f, 0.1f, 0.3f);  // no absorption, few real collisions: most flights reach their hit
  r.kg.session.medium_majorant = 64u;
  const ar::Real3 o0{ar::Real(0.125), ar::Real(-0.25), ar::Real(0.5)}, d{ar::Real(0.25), ar::Real(-0.5), ar::Real(2)};
  const ar::Real t0 = ar::Real(0.75), back0 = ar::Real(0.25);
  for (uint32_t i = 0; i < StageRig::kN; ++i) {
    r.ray_o[i] = o0, r.ray_d[i] = d, r.hit_t[i] = t0, r.prev_dist[i] = back0;
    r.medium_rng[i] = ar::flight_rng_start(3u);
  }
  r.run();
  const auto close = [](const ar::Real3& a, const ar::Real3& b) {
    const double tol = 32.0 * double(std::numeric_limits<ar::Real>::epsilon()) * 2.0;  // coordinates below 2
    return std::fabs(double(a.x - b.x)) <= tol && std::fabs(double(a.y - b.y)) <= tol && std::fabs(double(a.z - b.z)) <= tol;
  };
  uint32_t checked = 0;
  for (uint32_t i = 0; i < 3u; ++i) {
    const bool reached = r.term[i] == 0u && r.next[i] != uint8_t(Stage::INTERSECT_CLOSEST);
    if (!reached || r.medium_rng[i] - ar::flight_rng_start(3u) < 2u) continue;  // a reach after null collisions only
    ++checked;
    INFO("item " << i << " collisions " << r.medium_rng[i] - ar::flight_rng_start(3u));
    CHECK(r.hit_t[i] < t0);  // the ray did move
    CHECK(close(r.ray_o[i] + d * r.hit_t[i], o0 + d * t0));
    CHECK(close(r.ray_o[i] - d * r.prev_dist[i], o0 - d * back0));
  }
  CHECK(checked > 0u);
}

// Sets ASTRORAY_TEST_<name> for the sessions created in its scope (the session reads hooks at create).
struct HookScope {
  const char* var;
  HookScope(const char* v, const char* value) : var(v) { REQUIRE(_putenv_s(var, value) == 0); }
  ~HookScope() { _putenv_s(var, ""); }
};

TEST_CASE("shade_volume/requeue ends the pass") {
  // 0006 D8, V6 third clause, 0002 D7: the slab with a grey scattering medium (sigma_t 4 per m, optical
  // depth 2) under ASTRORAY_TEST_MAJORANT=16, so a flight takes about 30 null collisions. At
  // ASTRORAY_TEST_MEDIUM_CAP=1 every one is a requeue into the SHADE_VOLUME segment, yet each pass
  // ends (the session converges) with a film bit-identical to the default cap's. With one path per
  // pass (1 x 1, 1 spp) each requeue is one more step: steps at cap 1 = steps at the default + requeues.
  HookScope k("ASTRORAY_TEST_MAJORANT", "16");
  const auto spec = [](uint32_t w, uint32_t h, uint32_t spp) {
    slab::SlabSpec s;
    for (int c = 0; c < 3; ++c) s.medium.sigma_a[c] = 1.0f, s.medium.sigma_s[c] = 3.0f;
    s.medium.g = 0.5f;
    s.width = w, s.height = h, s.spp = spp;
    s.max_bounces = s.volume_bounces = 16;
    return s;
  };
  struct Out {
    slab::Render r;
    PassCounters c;
  };
  const auto render = [&](const slab::SlabSpec& sp, bool capped) {
    std::unique_ptr<HookScope> cap;
    if (capped) cap = std::make_unique<HookScope>("ASTRORAY_TEST_MEDIUM_CAP", "1");
    slab::SlabRig rig(SceneConfig{2, {}, {}}, sp);
    std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
    slab::SlabSession s(rig, *dev);
    Out o{s.converge(), {}};
    o.c = s.session.pass_counters();
    return o;
  };
  // The image: 6 x 4 at 2 spp.
  {
    const Out a = render(spec(slab::kW, slab::kH, 2), false), b = render(spec(slab::kW, slab::kH, 2), true);
    REQUIRE(a.r.combined.size() == size_t(slab::kW) * slab::kH * 3);
    REQUIRE(b.r.combined.size() == a.r.combined.size());
    double sum = 0.0;
    for (float v : a.r.combined) sum += v;
    CHECK(sum / double(a.r.combined.size()) > 0.01);  // not black: identical bits mean something
    CHECK(std::memcmp(a.r.combined.data(), b.r.combined.data(), a.r.combined.size() * sizeof(float)) == 0);
    CHECK(b.r.volume_items > 4u * a.r.volume_items);
  }
  // One path per pass: every requeue adds exactly one step.
  const Out a = render(spec(1, 1, 1), false), b = render(spec(1, 1, 1), true);
  CHECK(std::memcmp(a.r.combined.data(), b.r.combined.data(), 3 * sizeof(float)) == 0);
  const uint64_t requeues = b.r.volume_items - a.r.volume_items;
  std::printf("shade_volume requeue pass: steps %llu at the default cap, %llu at cap 1, requeues %llu\n",
              (unsigned long long)a.c.steps, (unsigned long long)b.c.steps, (unsigned long long)requeues);
  CHECK(requeues > 0u);
  CHECK(b.c.steps == a.c.steps + requeues);
}

// The report entries of a session, as ar_session_report_entry orders them.
static std::vector<ar_report_entry> report_entries(Session& s) {
  s.report().snapshot();
  ar_report_info info{};
  REQUIRE(s.report().info(&info) == AR_OK);
  std::vector<ar_report_entry> out(info.entry_count);
  for (uint32_t i = 0; i < info.entry_count; ++i) REQUIRE(s.report().entry(i, &out[i]) == AR_OK);
  return out;
}

TEST_CASE("session/medium path limit raises one report entry") {
  // ASTRORAY_TEST_MAJORANT at D8's per-event limit (1,048,576): a flight of optical depth 2 draws more
  // tentative collisions than the event's blocks hold, so every camera path that reaches the slab's medium
  // ends VOLUME_LIMIT. One pixel at two samples is two passes with one path each: the session reads the
  // pass's counter after each resolve and raises under one key, so the entry's count is 2 and there is
  // no other entry of that code. The same slab at the production majorant (1) raises none.
  const auto spec = [] {
    slab::SlabSpec s;
    for (int c = 0; c < 3; ++c) s.medium.sigma_a[c] = 2.0f, s.medium.sigma_s[c] = 2.0f;
    s.width = 1, s.height = 1, s.spp = 2;
    s.max_bounces = s.volume_bounces = 4;
    return s;
  };
  const auto entries = [&](bool limit) {
    std::unique_ptr<HookScope> k;
    if (limit) k = std::make_unique<HookScope>("ASTRORAY_TEST_MAJORANT", "1048576");
    slab::SlabRig rig(SceneConfig{2, {}, {}}, spec());
    std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, 1);
    slab::SlabSession s(rig, *dev);
    s.converge();
    std::vector<ar_report_entry> hit;
    for (const ar_report_entry& e : report_entries(s.session))
      if (e.code == AR_REPORT_MEDIUM_PATH_LIMIT) hit.push_back(e);
    return hit;
  };
  const std::vector<ar_report_entry> with = entries(true);
  REQUIRE(with.size() == 1u);
  CHECK(with[0].count == 2u);
  CHECK(with[0].level == AR_LEVEL_WARNING);
  CHECK(entries(false).empty());
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
