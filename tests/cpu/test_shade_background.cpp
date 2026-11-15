// SPDX-License-Identifier: Apache-2.0
// shade_background (0004 D5, D6, D28; 0002 D8): a hand-built miss adds exactly beta * Lw to its
// slot and ends ESCAPED with its statistics; a non-finite contribution marks its sample invalid
// (counted by the resolve, n unchanged) and ends INVALID; and a CPU session on an empty scene with a constant world resolves every pixel to the
// world's display RGB.
// Ref: docs/decisions/0004-integrator.md D5, D6, D28; 0003 d22; 0008 d3, d12.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/film/film_layout.h"
#include "film/resolve.h"
#include "host/film/film_view.h"
#include "host/film/response_table.h"
#include "host/spectral/wavelength_table.h"
#include "stages/film_build_active.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/mesh.h"
#include "vm/ops.h"
#include "host/scene/edit.h"
#include "host/scene/scene.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"
#include "kernel/light/light_sampler.h"
#include "kernel/material/slot_resolve.h"
#include "kernel/stages/shade_background.h"
#include "kernel/vm/context.h"
#include "kernel_cpu/stage_cpu.h"


using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kSlots = 4;

// Four paths of a 4x1 film, one sample each, in a lights table of [decoy point, world, decoy
// point] so that finding the world is part of what is tested. The world's colour is a constant
// REFLECTANCE-semantic spectrum, which makes Lw = strength independent of the observer tables.
struct MissRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  ResponseTable response;
  WavelengthTable wavelengths;
  std::unique_ptr<Film> film;
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  ar::SceneTables tables{};
  std::vector<ar::LightRecord> lights;
  ar::LightSamplerView sampler{};
  std::vector<uint32_t> queue{0, 1, 2, 3}, active{0, 1, 2, 3};
  std::vector<uint8_t> next_stage = std::vector<uint8_t>(kSlots, uint8_t(ar::Stage::SHADE_BACKGROUND));
  std::vector<uint8_t> term = std::vector<uint8_t>(kSlots, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags{0, 1, 2, 3};
  std::vector<ar::U16x6> bounces = std::vector<ar::U16x6>(kSlots, ar::U16x6{{0, 0, 0, 0, 0, 0}});
  std::vector<ar::Spec> lambda_t = std::vector<ar::Spec>(kSlots), beta = std::vector<ar::Spec>(kSlots);
  ar::StepEntry step{0, kSlots, 0};

  explicit MissRig(float world_strength) : pool(ctx, 1), dev(ledger, pool) {
    FilmRequest req;
    req.max_width = kSlots;
    req.max_height = 1;
    req.slot_capacity = kSlots;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});

    // Flat tables, as in test_film_stats: a slot of lanes v / 256 resolves to x = v in every component.
    response.display_first = 0;
    response.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    response.y_w = 1.0;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) response.m_ws[i] = m[i];
    for (uint32_t c = 0; c < 3; ++c) {
      response.channels.push_back({2 * c, 2, ar::RESPONSE_ENERGY, 0});
      response.knot_nm.push_back(300.0f);
      response.knot_nm.push_back(900.0f);
      response.knot_value.push_back(1.0f);
      response.knot_value.push_back(1.0f);
    }
    wavelengths.edges = {360.0f, 616.0f};
    wavelengths.cdf = {0.0f, 1.0f};
    film->data().response = response_view(response);
    film->data().wavelengths = {wavelengths.edges.data(), wavelengths.cdf.data(), wavelengths.bins()};
    const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(1));
    ar::LightRecord decoy = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, ar::Real(0), ar::Real(100), c);
    lights = {decoy, ar::light_make_world(ar::Real(world_strength), c), decoy};
    sampler.light_count = uint32_t(lights.size());

    for (uint32_t i = 0; i < kSlots; ++i) {
      for (int l = 0; l < ar::kLanes; ++l) {
        lambda_t[i].v[l] = 450.0f + 30.0f * float(l);
        beta[i].v[l] = (0.25f + 0.5f * float(i)) * (1.0f + 0.25f * float(l));  // a different beta per lane
      }
      flags[i] = ar::set_hero_lane(0u, i);
    }
    bounces[2].v[0] = 3;

    path.next_stage = next_stage.data();
    path.term = term.data();
    path.flags = flags.data();
    path.bounces = bounces.data();
    path.lambda_t = lambda_t.data();
    path.beta = beta.data();
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.active_pixels = active.data();
    kg.film = &film->data();
    kg.tables = &tables;
    kg.lights = lights.data();
    kg.world_light = 1;  // the table is [decoy, world, decoy]
    kg.light_sampler = &sampler;
    kg.session.width = kSlots;
    kg.session.height = 1;
    kg.session.samples_per_pass = 1;
    kg.session.seed = 7u;
    kg.session.sampler_mode = ar::SAMPLER_SOBOL;
  }

  // One resolve of the four slots, as the session does after the pass drains.
  void resolve() {
    film->check_resolve_fits(1);
    cpu_film_resolve(kg, 0, ar::film_resolve_count(kg));
  }
  void run() { cpu_stage_shade_background(kg, step, 0, kSlots); }
  ar::Spec slot_value(uint32_t slot) const {
    ar::Spec s;
    for (int l = 0; l < ar::kLanes; ++l) s.v[l] = film->data().slot_L[4u * slot + uint32_t(l)];
    return s;
  }
  uint32_t hist(ar::Term t, uint32_t pixel) const {
    const ar::FilmData& f = film->data();
    return f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + pixel];
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_background/miss adds beta times the world radiance") {
  MissRig rig(2.5f);
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    const ar::Spec got = rig.slot_value(s);
    for (int l = 0; l < ar::kLanes; ++l) CHECK(got.v[l] == 2.5f * rig.beta[s].v[l]);  // Lw = strength exactly
    CHECK(rig.term[s] == uint8_t(ar::Term::ESCAPED));
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    CHECK(rig.film->data().slot_flags[s] == rig.flags[s]);
    CHECK(rig.hist(ar::Term::ESCAPED, s) == 1u);
    CHECK(rig.hist(ar::Term::INVALID, s) == 0u);
  }
  // Path-end statistics: the total bounce count of the path reaches the film (slot 2 had 3).
  const ar::FilmData& f = rig.film->data();
  CHECK(f.count[uint64_t(f.bounce_max) * f.plane_stride + 2] == 3u);
  CHECK(f.count[uint64_t(f.bounce_paths) * f.plane_stride + 2] == 1u);
}

TEST_CASE("shade_background/a scene without a world is black and ends escaped") {
  MissRig rig(2.5f);
  rig.kg.world_light = ar::kNoLight;
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    for (int l = 0; l < ar::kLanes; ++l) CHECK(rig.slot_value(s).v[l] == 0.0f);
    CHECK(rig.term[s] == uint8_t(ar::Term::ESCAPED));
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    CHECK(rig.hist(ar::Term::ESCAPED, s) == 1u);
  }
}

TEST_CASE("shade_background/the cpu session device runs the stage") {
  ContextState ctx;
  std::shared_ptr<Device> dev = make_cpu_session_device(ctx, 1);
  CHECK(static_cast<const CpuDevice&>(*dev).stage_fn(ar::Stage::SHADE_BACKGROUND) == &cpu_stage_shade_background);
}

TEST_CASE("shade_background/non-finite contribution ends invalid and is counted, not averaged") {
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  {
    MissRig rig(inf);  // an infinite world
    rig.run();
    for (uint32_t s = 0; s < kSlots; ++s) {
      for (int l = 0; l < ar::kLanes; ++l) CHECK(!ar::film_value_valid(float(rig.slot_value(s).v[l])));
      CHECK(rig.term[s] == uint8_t(ar::Term::INVALID));
      CHECK(rig.hist(ar::Term::INVALID, s) == 1u);
      CHECK(rig.hist(ar::Term::ESCAPED, s) == 0u);
      CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    }
    rig.resolve();  // 0008 d6: counted in the invalid counter, n does not move
    for (uint32_t p = 0; p < kSlots; ++p) {
      CHECK(rig.film->data().invalid[p] == 1u);
      CHECK(rig.film->data().n[p] == 0u);
    }
  }
  {
    MissRig rig(1.0f);  // a NaN throughput on one path only
    rig.beta[1].v[2] = nan;
    rig.run();
    CHECK(rig.term[1] == uint8_t(ar::Term::INVALID));
    CHECK(!ar::film_sample_valid(rig.slot_value(1)));
    CHECK(rig.term[0] == uint8_t(ar::Term::ESCAPED));
    CHECK(rig.slot_value(0).v[0] == rig.beta[0].v[0]);
    rig.resolve();
    CHECK(rig.film->data().invalid[1] == 1u);
    CHECK(rig.film->data().n[1] == 0u);
    for (uint32_t p : {0u, 2u, 3u}) {
      CHECK(rig.film->data().invalid[p] == 0u);
      CHECK(rig.film->data().n[p] == 1u);
    }
  }
}

TEST_CASE("shade_background/the table records the world's index") {
  // [emissive triangle, world, point]: the index is the world's position, not 0 or the last.
  const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(1));
  const std::vector<float> cmf(3 * ar::kObsSamples, 0.0f), illum(ar::kObsSamples, 1.0f);
  const ar::ObserverView obs{cmf.data(), illum.data()};
  const ar::SceneSphere sphere{ar::Real3{0, 0, 0}, ar::Real(10)};
  LightInput tri, world, point;
  tri.record = ar::light_make_triangle(ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 0, 0, 0, ar::Real(1), c);
  world.record = ar::light_make_world(ar::Real(1), c);
  point.record = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 1}, ar::Real(0), ar::Real(1), c);
  CHECK(build_light_table({tri, world, point}, sphere, obs, nullptr)->world_light == 1u);
  CHECK(build_light_table({tri, point}, sphere, obs, nullptr)->world_light == ar::kNoLight);
  CHECK_THROWS_AS(build_light_table({tri, world, point, world}, sphere, obs, nullptr), std::runtime_error);
}

}  // TEST_SUITE

namespace {

constexpr uint32_t kW = 6, kH = 4, kSpp = 64;
constexpr float kStrength = 0.8f;
constexpr float kRgb[3] = {1.0f, 0.5f, 0.25f};

// An empty scene has no hit: INTERSECT_CLOSEST is a test stub that marks every camera ray a miss.
// The real end-to-end check (production acceleration structures, an empty scene through Embree)
// lands with task 114; until then production code has no "no accel means miss" fallback.
void stub_all_miss(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i)
    ar::ps_next_stage(kg, kg.main_queue[step.start + i]) = static_cast<uint8_t>(ar::Stage::SHADE_BACKGROUND);
}

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

struct WorldGraph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  int add(ar::Op op, std::vector<float> fp) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    f.insert(f.end(), fp.begin(), fp.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  WorldGraph() {  // Background: constant colour times strength
    const int c = add(ar::Op::CONST_F32X3, {kRgb[0], kRgb[1], kRgb[2]}), s = add(ar::Op::CONST_F32, {kStrength});
    const int e = add(ar::Op::EMIT_LE, {});
    links.push_back({uint32_t(c), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(s), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    return d;
  }
};

}  // namespace

TEST_SUITE("gate") {

struct Rendered {
  std::vector<float> mean, var;
  std::vector<uint32_t> invalid;
  uint32_t world_light = 0;
  std::vector<uint32_t> n;
  double radiance_scale = 1.0, inv_radiance_scale = 1.0;
};

// Renders the empty scene to convergence; without a world the scene has no world light at all.
Rendered render_empty_scene(bool with_world) {
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
  {
    const WorldGraph g;
    const ar_graph_desc d = g.desc();
    ar_world_desc w = {};
    w.surface_graph = 40;
    ar_settings s{};
    s.samples = s.viewport_samples = kSpp;
    s.max_bounces = 4;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    s.seed = 11;
    auto e = scene.begin_edit();
    REQUIRE(e);
    if (with_world) {
      REQUIRE(scene.materials->set_graph(*e, 40, &d) == AR_OK);
      REQUIRE(scene.materials->set_world(*e, &w) == AR_OK);
    }
    REQUIRE(e->set_settings(&s) == AR_OK);
    uint64_t generation = 0;
    REQUIRE(e->commit(&generation) == AR_OK);
    scene.generations().wait_idle();
    REQUIRE(scene.generations().active() == generation);
  }

  Ledger ledger;
  ThreadPool pool(ctx, 2);
  CpuDevice dev(ledger, pool);
  std::shared_ptr<Device> production = make_cpu_session_device(ctx, 1);
  dev.register_scheduler_kernels();
  dev.register_kernel(KernelId::GENERATE_CAMERA,
                      static_cast<const CpuDevice&>(*production).kernel_fn(KernelId::GENERATE_CAMERA));
  dev.register_stage(ar::Stage::INTERSECT_CLOSEST, &stub_all_miss);
  dev.register_stage(ar::Stage::SHADE_BACKGROUND, &cpu_stage_shade_background);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session session(scene, dev, o);
  session.set_view(view_of(kW, kH));
  session.start();
  REQUIRE(session.wait(60000));
  REQUIRE(session.progress().state == AR_STATE_CONVERGED);

  Rendered out;
  std::vector<float>& mean = out.mean;
  std::vector<float>& var = out.var;
  std::vector<uint32_t>& invalid = out.invalid;
  out.world_light = scene.lights()->world_light;
  out.radiance_scale = scene.lights()->radiance_scale;
  session.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    const auto passes = list_passes(c.film.layout());
    for (const PassDesc& p : passes) {
      if (p.name != "Combined") continue;
      mean.resize(pass_float_count(p, AR_QUANTITY_MEAN, view.width(), view.height()));
      var.resize(pass_float_count(p, AR_QUANTITY_VARIANCE, view.width(), view.height()));
      read_pass(view, p, AR_QUANTITY_MEAN, uint32_t(mean.size()), mean.data());
      read_pass(view, p, AR_QUANTITY_VARIANCE, uint32_t(var.size()), var.data());
    }
    invalid = view.invalid_counts();
    out.n = view.sample_counts();
    out.inv_radiance_scale = c.film.data().inv_radiance_scale;
  });
  REQUIRE(mean.size() == size_t(kW) * kH * 3);
  return out;
}

TEST_CASE("shade_background/empty scene shows world") {
  const Rendered r = render_empty_scene(true);
  const std::vector<float>& mean = r.mean;
  const std::vector<float>& var = r.var;
  const std::vector<uint32_t>& invalid = r.invalid;
  CHECK(r.world_light == 0u);  // no emissive triangles or lamps: the world is the only light
  // s != 1, so applying it twice or not dividing it out would show in the means below.
  CHECK(r.radiance_scale != 1.0);
  CHECK(r.inv_radiance_scale == 1.0 / r.radiance_scale);
  for (uint32_t n : r.n) CHECK(n == kSpp);  // an invalid sample would be missing from n

  // The display RGB of the world is its colour times strength (a white-balanced display group,
  // exposure 1). Every pixel and component must agree within 5 standard errors of its own mean;
  // the float epsilon only covers the film's fp32 slot rounding.
  const double expect[3] = {kRgb[0] * kStrength, kRgb[1] * kStrength, kRgb[2] * kStrength};
  uint32_t outside = 0, bad_invalid = 0;
  double worst = 0.0, pooled[3] = {0, 0, 0}, pooled_var[3] = {0, 0, 0};
  for (uint32_t p = 0; p < kW * kH; ++p) {
    bad_invalid += invalid[p] != 0u;
    for (uint32_t c = 0; c < 3; ++c) {
      const double m = mean[p * 3 + c], se = std::sqrt(double(var[p * 3 + c]));
      pooled[c] += m / double(kW * kH);
      pooled_var[c] += double(var[p * 3 + c]) / double(kW * kH) / double(kW * kH);
      const double z = std::fabs(m - expect[c]) / (se + 1e-7);
      worst = std::fmax(worst, z);
      outside += std::fabs(m - expect[c]) > 5.0 * se + 1e-6;
    }
  }
  std::printf("shade_background: world display RGB expected (%.4f %.4f %.4f), pooled mean (%.4f %.4f %.4f), "
              "pooled SE (%.5f %.5f %.5f), worst pixel z = %.2f, %u of %u components outside 5 SE\n",
              expect[0], expect[1], expect[2], pooled[0], pooled[1], pooled[2], std::sqrt(pooled_var[0]),
              std::sqrt(pooled_var[1]), std::sqrt(pooled_var[2]), worst, outside, kW * kH * 3);
  CHECK(outside == 0u);
  CHECK(bad_invalid == 0u);
  for (uint32_t c = 0; c < 3; ++c) CHECK(std::fabs(pooled[c] - expect[c]) <= 5.0 * std::sqrt(pooled_var[c]) + 1e-6);
}

TEST_CASE("shade_background/empty scene without a world is black") {
  const Rendered r = render_empty_scene(false);
  CHECK(r.world_light == ar::kNoLight);
  for (float m : r.mean) CHECK(m == 0.0f);
  for (uint32_t i : r.invalid) CHECK(i == 0u);
}

}  // TEST_SUITE
