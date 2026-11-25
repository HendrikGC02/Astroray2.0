// SPDX-License-Identifier: Apache-2.0
// shade_background (0004 D5, D6, D28; 0002 D8): a hand-built miss adds exactly beta * Lw to its
// slot and ends ESCAPED with its statistics; a non-finite contribution marks its sample invalid
// (counted by the resolve, n unchanged) and ends INVALID; and a CPU session on an empty scene with a constant world resolves every pixel to the
// world's display RGB.
// Ref: docs/decisions/0004-integrator.md D5, D6, D28; 0003 d22; 0008 d3, d12.
#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
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
  std::vector<uint32_t> sample_id{0, 1, 2, 3};  // the film slot of each pool slot (0004 D5)
  std::vector<ar::U16x6> bounces = std::vector<ar::U16x6>(kSlots, ar::U16x6{{0, 0, 0, 0, 0, 0}});
  std::vector<ar::Spec> lambda_t = std::vector<ar::Spec>(kSlots), beta = std::vector<ar::Spec>(kSlots);
  // r_u = 1: the unweighted miss divides by avg(r_u) (074), so the expected values stay beta Le.
  std::vector<ar::Spec> r_u = std::vector<ar::Spec>(kSlots, ar::spec_fill(1.0f));
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
    flags[2] |= ar::LAST_BOUNCE_SPECULAR;  // after a delta bounce: weight 1 / avg(r_u), no world MIS

    path.sample_id = sample_id.data();
    path.next_stage = next_stage.data();
    path.term = term.data();
    path.flags = flags.data();
    path.bounces = bounces.data();
    path.lambda_t = lambda_t.data();
    path.beta = beta.data();
    path.r_u = r_u.data();
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

// ---- program worlds: an Environment Texture over the ray direction (0005 item 32) ----------------------

constexpr uint32_t kEnvW = 64, kEnvH = 32;

// A float RGBA image (rows bottom-up) with the world compiled against it: the graph is LOAD_CTX_P ->
// TEX_ENVIRONMENT (image key 7) -> EMIT_LE with a constant strength, as the addon lowers an Environment
// Texture on a Background. `tables` is what the kernel reads (image slot 0 is the image).
struct EnvFixture {
  MaterialStore store{std::make_shared<Ledger>()};
  ObserverTables obs;
  std::array<float, ar::kSrgbTableSize> srgb{};
  std::vector<float> texels;
  ar::DeviceImageView view{};
  ar::SceneTables tables{};
  ar::SceneData scene{};
  StoredGraph graph;
  WorldOutput world;

  EnvFixture(uint32_t w, uint32_t h, std::vector<float> rgba, uint32_t mode, float strength) : texels(std::move(rgba)) {
    DataBlob blob;
    REQUIRE(find_data("spectral/observer.bin", &blob));
    REQUIRE(parse_observer(blob.data, blob.size, &obs).empty());
    ar::srgb_decode_table(srgb.data());
    view.texels = uint64_t(uintptr_t(texels.data()));
    view.width = w, view.height = h, view.channels = 4, view.format = uint32_t(ar::TexFormat::FLOAT);
    view.encoding = uint32_t(ar::TexEncoding::LINEAR);
    tables.rgb2spec = &store.table();
    tables.observer = {&obs.cmf[0][0], obs.d65};
    tables.images = &view;
    tables.image_count = 1;
    tables.srgb_decode = srgb.data();
    // LOAD_CTX_P, TEX_ENVIRONMENT {key 7, mode}, CONST_F32 {strength}, EMIT_LE.
    graph.f32 = {strength};
    graph.i32 = {7, int32_t(mode)};
    graph.nodes.assign(4, ar_graph_node{});
    graph.nodes[0].op = uint32_t(ar::Op::LOAD_CTX_P);
    graph.nodes[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), graph.nodes[1].i32_first = 0, graph.nodes[1].i32_count = 2;
    graph.nodes[2].op = uint32_t(ar::Op::CONST_F32), graph.nodes[2].f32_first = 0, graph.nodes[2].f32_count = 1;
    graph.nodes[3].op = uint32_t(ar::Op::EMIT_LE);
    graph.links = {{0u, 0u, 1u, 0u}, {1u, 0u, 3u, 0u}, {2u, 0u, 3u, 1u}};
    const ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7u ? (*slot = 0u, true) : false; };
    const ImageMeanFn means = [](uint32_t, std::array<float, 3>* rgb) { return (*rgb = {1.0f, 1.0f, 1.0f}, true); };
    ar_world_desc desc = {};
    desc.surface_graph = 1;
    world = compile_world(desc, &graph, store.table(), &means, &slots);
    REQUIRE_MESSAGE(!world.magenta, "the environment world compiled to magenta");
    REQUIRE(world.program.size() > 2);
    REQUIRE(world.program[2] != 0u);  // the emission entry
    scene.world_program = world.program.data();
    scene.world_program_words = uint32_t(world.program.size());
    scene.world_constants = &world.block;
  }

  // What a constant world of record strength `strength` radiates for colour `c` at `lam`: the folded
  // route a program must equal.
  ar::Spec constant_le(std::array<float, 3> c, float strength, const ar::Lambda& lam) const {
    ar::SpectrumRef ref;
    uint32_t flags = 0;
    ar::make_rgb_ref(store.table(), ar::Rgb{{ar::Real(c[0]), ar::Real(c[1]), ar::Real(c[2])}}, ar::SpectrumSemantic::ILLUMINANT,
                     &ref, &flags);
    return ar::light_emission(ar::light_make_world(ar::Real(strength), ref), lam, tables.observer);
  }
};

// The unit direction an equirectangular texture shows at (u, v), Blender's convention written the other
// way round: u = 0.5 is +x, +y is u = 0.25, and v = 1 is straight up (+z).
std::array<double, 3> env_direction(double u, double v) {
  const double pi = 3.14159265358979323846;
  const double phi = pi - 2.0 * pi * u, theta = pi * (1.0 - v);
  return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

// A miss rig whose world record names the program of `fx`.
struct EnvRig : MissRig {
  std::vector<ar::Real3> ray_d = std::vector<ar::Real3>(kSlots);
  std::vector<uint8_t> ray_type = std::vector<uint8_t>(kSlots, 0);
  EnvRig(EnvFixture& fx, float record_strength) : MissRig(record_strength) {
    const ar::SpectrumRef one = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1));
    lights[1] = ar::light_make_world(ar::Real(record_strength), one, ar::kLightWorldProgram);
    tables = fx.tables;
    kg.scene = &fx.scene;
    path.ray_d = ray_d.data();
    path.ray_type = ray_type.data();
  }
  ar::Lambda lam(uint32_t slot) const {
    ar::Lambda l;
    for (int i = 0; i < ar::kLanes; ++i) l.nm[i] = lambda_t[slot].v[i];
    return l;
  }
  void set_dir(uint32_t slot, std::array<double, 3> d) { ray_d[slot] = {ar::Real(d[0]), ar::Real(d[1]), ar::Real(d[2])}; }
};

// Texel (ix, iy) of the test image: every texel its own colour, so a wrong position or row order shows.
std::array<float, 3> env_texel(uint32_t ix, uint32_t iy) {
  return {0.1f + 0.9f * float(ix) / float(kEnvW), 0.1f + 0.9f * float(iy) / float(kEnvH), 0.25f + 0.5f * float((ix + iy) & 1u)};
}
std::vector<float> env_image() {
  std::vector<float> px(size_t(kEnvW) * kEnvH * 4, 1.0f);
  for (uint32_t y = 0; y < kEnvH; ++y)
    for (uint32_t x = 0; x < kEnvW; ++x) {
      const std::array<float, 3> c = env_texel(x, y);
      for (int k = 0; k < 3; ++k) px[(size_t(y) * kEnvW + x) * 4 + size_t(k)] = c[size_t(k)];
    }
  return px;
}

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

TEST_CASE("shade_background/writes land on the sample's film slot") {
  // A refill that handed out the pool slots in another order: pool slot i carries sample p[i].
  MissRig rig(2.5f);
  const uint32_t p[kSlots] = {2, 3, 0, 1};
  for (uint32_t i = 0; i < kSlots; ++i) rig.sample_id[i] = p[i];
  rig.beta[1].v[0] = std::numeric_limits<float>::quiet_NaN();  // pool slot 1 = sample 3: invalid
  rig.run();
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t s = p[i];
    CHECK(rig.film->data().slot_flags[s] == rig.flags[i]);
    if (i == 1) {
      CHECK(!ar::film_sample_valid(rig.slot_value(s)));
      CHECK(rig.hist(ar::Term::INVALID, s) == 1u);
    } else {
      for (int l = 0; l < ar::kLanes; ++l) CHECK(rig.slot_value(s).v[l] == 2.5f * rig.beta[i].v[l]);
      CHECK(rig.hist(ar::Term::ESCAPED, s) == 1u);
    }
  }
  // Path slot 2 had 3 bounces and carries sample 0: the bounce maximum lands on pixel 0.
  const ar::FilmData& f = rig.film->data();
  CHECK(f.count[uint64_t(f.bounce_max) * f.plane_stride + 0] == 3u);
  CHECK(f.count[uint64_t(f.bounce_max) * f.plane_stride + 2] == 0u);
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

TEST_CASE("shade_background/environment texel direction") {
  // One bright texel at (ix0, iy0) in a dark 64 x 32 map. The escaped ray along the direction the
  // convention gives for that texel's centre reads it; 90 degrees round z, mirrored in z and opposite,
  // it reads the dark ground instead.
  const uint32_t ix0 = 40, iy0 = 22;
  const std::array<float, 3> dark = {0.05f, 0.05f, 0.05f}, bright = {6.0f, 3.0f, 1.5f};
  std::vector<float> px(size_t(kEnvW) * kEnvH * 4, 1.0f);
  for (size_t i = 0; i < size_t(kEnvW) * kEnvH; ++i)
    for (size_t k = 0; k < 3; ++k) px[4 * i + k] = dark[k];
  for (size_t k = 0; k < 3; ++k) px[(size_t(iy0) * kEnvW + ix0) * 4 + k] = bright[k];
  const float strength = 0.8f, s = 4.0f;  // s is a power of two, as the radiance scale always is
  EnvFixture fx(kEnvW, kEnvH, px, 0u, strength);
  EnvRig rig(fx, s);
  const std::array<double, 3> d = env_direction((double(ix0) + 0.5) / kEnvW, (double(iy0) + 0.5) / kEnvH);
  rig.set_dir(0, d);
  rig.set_dir(1, {-d[1], d[0], d[2]});
  rig.set_dir(2, {d[0], d[1], -d[2]});
  rig.set_dir(3, {-d[0], -d[1], -d[2]});
  rig.run();
  for (uint32_t i = 0; i < kSlots; ++i) {
    const ar::Spec le = fx.constant_le(i == 0 ? bright : dark, strength * s, rig.lam(i));
    const ar::Spec got = rig.slot_value(i);
    INFO("slot " << i);
    for (int l = 0; l < ar::kLanes; ++l) CHECK(got.v[l] == le.v[l] * rig.beta[i].v[l]);
    CHECK(rig.term[i] == uint8_t(ar::Term::ESCAPED));
  }
  // The bright texel is far brighter than the ground, so the check above cannot pass by luck.
  CHECK(rig.slot_value(0).v[1] > 20.0f * rig.slot_value(1).v[1] * rig.beta[0].v[1] / rig.beta[1].v[1]);
}

TEST_CASE("shade_background/environment texture reads each texel at its direction") {
  const float strength = 1.0f, s = 2.0f;
  EnvFixture closest(kEnvW, kEnvH, env_image(), 0u, strength);
  EnvRig rig(closest, s);
  const ar::LightRecord* world = ar::world_light(rig.kg);
  REQUIRE(world != nullptr);
  uint32_t bad = 0;
  for (uint32_t iy = 0; iy < kEnvH; ++iy)
    for (uint32_t ix = 0; ix < kEnvW; ++ix) {
      rig.set_dir(0, env_direction((double(ix) + 0.5) / kEnvW, (double(iy) + 0.5) / kEnvH));
      ar::Spec le;
      REQUIRE(ar::world_radiance(rig.kg, *world, 0, rig.lam(0), &le));
      const ar::Spec want = closest.constant_le(env_texel(ix, iy), strength * s, rig.lam(0));
      for (int l = 0; l < ar::kLanes; ++l) bad += le.v[l] != want.v[l];
    }
  CHECK(bad == 0u);  // every texel at its own direction, bit for bit

  // Linear at a texel centre is that texel (to the rounding of the direction); across the longitude
  // seam (-x) it is continuous, which only a repeating lookup gives.
  EnvFixture linear(kEnvW, kEnvH, env_image(), 1u, strength);
  EnvRig lrig(linear, s);
  const ar::LightRecord* lworld = ar::world_light(lrig.kg);
  double worst = 0.0;
  for (uint32_t iy = 1; iy + 1 < kEnvH; iy += 3)
    for (uint32_t ix = 0; ix < kEnvW; ix += 5) {
      lrig.set_dir(0, env_direction((double(ix) + 0.5) / kEnvW, (double(iy) + 0.5) / kEnvH));
      ar::Spec le;
      REQUIRE(ar::world_radiance(lrig.kg, *lworld, 0, lrig.lam(0), &le));
      const ar::Spec want = linear.constant_le(env_texel(ix, iy), strength * s, lrig.lam(0));
      for (int l = 0; l < ar::kLanes; ++l) worst = std::max(worst, std::fabs(double(le.v[l]) - double(want.v[l])) / double(want.v[l]));
    }
  CHECK(worst < 2e-4);
  ar::Spec above, below;
  lrig.set_dir(0, {-1.0, 1e-4, 0.0});
  lrig.set_dir(1, {-1.0, -1e-4, 0.0});
  REQUIRE(ar::world_radiance(lrig.kg, *lworld, 0, lrig.lam(0), &above));
  REQUIRE(ar::world_radiance(lrig.kg, *lworld, 1, lrig.lam(0), &below));
  double seam = 0.0;
  for (int l = 0; l < ar::kLanes; ++l) seam = std::max(seam, std::fabs(double(above.v[l]) - double(below.v[l])) / double(above.v[l]));
  CHECK(seam < 1e-2);  // texels 63 and 0 differ by 7x in red: an EXTEND lookup would jump
}

TEST_CASE("world/table colour is refused at compile") {
  // The world graph's one constant emission is a TABLE: compile_world renders the world magenta with
  // one entry, so the scene's light gather (which throws on a TABLE colour) never sees it.
  const float f[] = {0.8f, 360.0f, 0.5f, 830.0f, 0.5f, 2.0f};  // table scale and two points, then the strength
  ar_graph_node n[3] = {};
  n[0].op = uint32_t(ar::Op::SPECTRUM_TABLE), n[0].f32_first = 0, n[0].f32_count = 5;
  n[1].op = uint32_t(ar::Op::CONST_F32), n[1].f32_first = 5, n[1].f32_count = 1;
  n[2].op = uint32_t(ar::Op::EMIT_LE);
  const ar_graph_link links[2] = {{0, 0, 2, 0}, {1, 0, 2, 1}};
  StoredGraph sg;
  sg.nodes.assign(n, n + 3), sg.links.assign(links, links + 2), sg.f32.assign(f, f + 6);
  MaterialStore store(std::make_shared<Ledger>());
  ar_world_desc d = {};
  d.surface_graph = 1;
  const WorldOutput w = compile_world(d, &sg, store.table());
  CHECK(w.magenta);
  CHECK(w.color.kind != uint8_t(ar::SpectrumKind::TABLE));
  CHECK(w.program.empty());
  REQUIRE(w.entries.size() == 1);
  CHECK(w.entries[0].code == AR_REPORT_NODE_UNSUPPORTED);
  CHECK(w.entries[0].detail.find("SPECTRUM_TABLE") != std::string::npos);
  MeshStore meshes;
  const MaterialArrays mats;
  CHECK_NOTHROW(gather_scene_lights(nullptr, meshes, mats, &w, true));
}
TEST_CASE("shade_background/program world equals folded") {
  // A constant world through the program path (a 1 x 1 image of the colour, Closest) equals the
  // world the compiler folds to a constant, bit for bit, at every lane of every path.
  const float s = 4.0f;
  const std::array<float, 3> rgb = {1.0f, 0.5f, 0.25f};
  ar_graph_node n[3] = {};
  n[0].op = uint32_t(ar::Op::CONST_F32X3), n[0].f32_first = 0, n[0].f32_count = 3;
  n[1].op = uint32_t(ar::Op::CONST_F32), n[1].f32_first = 3, n[1].f32_count = 1;
  n[2].op = uint32_t(ar::Op::EMIT_LE);
  const float f[4] = {rgb[0], rgb[1], rgb[2], 0.8f};
  const ar_graph_link links[2] = {{0, 0, 2, 0}, {1, 0, 2, 1}};
  StoredGraph sg;
  sg.nodes.assign(n, n + 3), sg.links.assign(links, links + 2), sg.f32.assign(f, f + 4);
  EnvFixture fx(1, 1, {rgb[0], rgb[1], rgb[2], 1.0f}, 0u, 0.8f);
  ar_world_desc desc = {};
  desc.surface_graph = 1;
  const WorldOutput folded = compile_world(desc, &sg, fx.store.table());
  REQUIRE((folded.program.empty() && !folded.magenta && folded.strength == 0.8f));

  MissRig constant(1.0f);
  constant.tables = fx.tables;
  constant.lights[1] = ar::light_make_world(ar::Real(folded.strength * s), folded.color);
  EnvRig program(fx, s);
  for (uint32_t i = 0; i < kSlots; ++i) program.set_dir(i, {0.3 * double(i) - 0.4, 0.8, -0.2 + 0.25 * double(i)});
  constant.run();
  program.run();
  for (uint32_t i = 0; i < kSlots; ++i)
    for (int l = 0; l < ar::kLanes; ++l) {
      INFO("slot " << i << " lane " << l);
      CHECK(program.slot_value(i).v[l] == constant.slot_value(i).v[l]);
      CHECK(program.slot_value(i).v[l] > 0.0f);
    }
}

TEST_CASE("shade_background/a program world that cannot run fails the sample") {
  EnvFixture fx(kEnvW, kEnvH, env_image(), 0u, 1.0f);
  EnvRig rig(fx, 1.0f);
  for (uint32_t i = 0; i < kSlots; ++i) rig.set_dir(i, {1.0, 0.0, 0.0});
  fx.scene.world_program = nullptr;  // the host failed to upload it: loud, never a black or constant world
  rig.run();
  for (uint32_t i = 0; i < kSlots; ++i) {
    CHECK(rig.term[i] == uint8_t(ar::Term::INVALID));
    CHECK(!ar::film_sample_valid(rig.slot_value(i)));
  }
}

TEST_CASE("shade_background/a program world has pmf 0 and nothing samples it") {
  const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1));
  const std::vector<float> cmf(3 * ar::kObsSamples, 1.0f), illum(ar::kObsSamples, 1.0f);
  const ar::ObserverView obs{cmf.data(), illum.data()};
  const ar::SceneSphere sphere{ar::Real3{0, 0, 0}, ar::Real(10)};
  LightInput point, world;
  point.record = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 1}, ar::Real(0), ar::Real(5), c);
  world.record = ar::light_make_world(ar::Real(1), c, ar::kLightWorldProgram);
  {
    const auto t = build_light_table({point, world}, sphere, obs, nullptr);
    REQUIRE(t->world_light == 1u);
    CHECK(t->importance[1] == 0.0);
    CHECK(t->pmf[1] == 0.0f);
    CHECK(t->pmf[0] == 1.0f);
    CHECK(t->bins.size() == 1u);  // the alias table holds the point only
    const ar::LightSamplerView v = t->view();
    ar::LightSampleContext ctx{};
    uint32_t picked_world = 0;
    for (uint32_t i = 0; i < 2000; ++i) picked_world += ar::light_sampler_sample(v, ctx, i * 2654435761u, float(i) / 2000.0f, false).light == 1u;
    CHECK(picked_world == 0u);
    CHECK(ar::light_sampler_pmf(v, ctx, 1, false) == 0.0f);
    CHECK(ar::light_sampler_pmf(v, ctx, 1, true) == 0.0f);
  }
  // The same world with its constant record is sampled, so the zero comes from the flag.
  world.record = ar::light_make_world(ar::Real(1), c);
  CHECK(build_light_table({point, world}, sphere, obs, nullptr)->pmf[1] > 0.0f);
  // A program world alone: no importance anywhere, which is a table, not an error.
  world.record = ar::light_make_world(ar::Real(1), c, ar::kLightWorldProgram);
  CHECK(build_light_table({world}, sphere, obs, nullptr)->bins.empty());
}

TEST_CASE("shade_background/the world recompiles when its image changes") {
  // LOAD_CTX_P -> TEX_ENVIRONMENT (image 5, Closest) -> EMIT_LE with strength 1, world graph 40.
  const int32_t i32[2] = {5, 0};
  const float f32[1] = {1.0f};
  ar_graph_node n[4] = {};
  n[0].op = uint32_t(ar::Op::LOAD_CTX_P);
  n[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), n[1].i32_first = 0, n[1].i32_count = 2;
  n[2].op = uint32_t(ar::Op::CONST_F32), n[2].f32_first = 0, n[2].f32_count = 1;
  n[3].op = uint32_t(ar::Op::EMIT_LE);
  const ar_graph_link links[3] = {{0, 0, 1, 0}, {1, 0, 3, 0}, {2, 0, 3, 1}};
  ar_graph_desc gd = {};
  gd.node_count = 4, gd.link_count = 3, gd.f32_count = 1, gd.i32_count = 2;
  gd.nodes = n, gd.links = links, gd.params_f32 = f32, gd.params_i32 = i32;

  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  scene.meshes = std::make_shared<MeshStore>();
  scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
  MaterialStore& store = *scene.materials;
  const auto commit = [&](const std::function<void(Edit&)>& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    store.end_edit();
    e.reset();
    scene.generations().wait_idle();
  };
  const auto put_image = [&](Edit& e, const std::vector<float>& rgba) {
    ar_image_desc d = {};
    d.key = 5, d.source = AR_IMAGE_SOURCE_PIXELS, d.color_interop = AR_COLOR_INTEROP_LIN_REC709, d.width = 2, d.height = 1;
    ar_image_buffers b = {};
    REQUIRE(store.image_begin(e, &d, &b) == AR_OK);
    REQUIRE(b.pixels_count == rgba.size());
    std::memcpy(*b.pixels, rgba.data(), rgba.size() * sizeof(float));
    REQUIRE(e.item_commit(5) == AR_OK);
  };
  // The word after the TEX_ENVIRONMENT instruction of the emission entry: the image slot.
  const auto slot_word = [&]() {
    const auto a = store.arrays();
    REQUIRE(a);
    const std::vector<uint32_t>& p = a->world_program;
    REQUIRE(p.size() > 4);
    for (size_t pc = p[2]; pc + 1 < p.size(); ++pc)
      if ((p[pc] & 0xffu) == uint32_t(ar::Op::TEX_ENVIRONMENT)) return p[pc + 1];
    FAIL("no TEX_ENVIRONMENT in the world program");
    return 0u;
  };
  const auto unreadable = [&](const std::shared_ptr<const WorldOutput>& w) {
    uint32_t count = 0;
    for (const MaterialEntry& e : w->entries) count += e.code == AR_REPORT_IMAGE_UNREADABLE;
    return count;
  };

  // The world arrives before its image: the program holds "no image", reported, and renders magenta.
  commit([&](Edit& e) {
    REQUIRE(store.set_graph(e, 40, &gd) == AR_OK);
    ar_world_desc w = {};
    w.surface_graph = 40;
    REQUIRE(store.set_world(e, &w) == AR_OK);
  });
  auto w0 = store.world();
  REQUIRE(w0);
  CHECK(!w0->program.empty());
  CHECK(w0->images == std::vector<uint32_t>{5u});
  CHECK(slot_word() == ar::kTexNoImage);
  CHECK(unreadable(w0) == 1u);
  CHECK(store.arrays()->world_constants.size() == 1u);

  // Setting the image alone recompiles the world: it now holds the image's slot, with no report.
  commit([&](Edit& e) { put_image(e, {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f}); });
  auto w1 = store.world();
  CHECK(w1 != w0);
  CHECK(slot_word() == 0u);
  CHECK(unreadable(w1) == 0u);

  // Replacing the image recompiles it again (a new output, same slot); an edit that touches neither
  // keeps the very same output.
  commit([&](Edit& e) { put_image(e, {0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f}); });
  auto w2 = store.world();
  CHECK(w2 != w1);
  CHECK(slot_word() == 0u);
  commit([&](Edit& e) {
    ar_settings s{};
    s.samples = s.viewport_samples = 1;
    s.max_bounces = 1;
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    REQUIRE(e.set_settings(&s) == AR_OK);
  });
  CHECK(store.world() == w2);

  // Removing the image sends it back to magenta; a constant graph under the same key drops the program.
  commit([&](Edit& e) {
    REQUIRE(e.remove(AR_KIND_IMAGE, 5) == AR_OK);
    REQUIRE(store.note_remove(e, AR_KIND_IMAGE, 5) == AR_OK);
  });
  CHECK(slot_word() == ar::kTexNoImage);
  CHECK(unreadable(store.world()) == 1u);
  const float cf[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ar_graph_node cn[3] = {};
  cn[0].op = uint32_t(ar::Op::CONST_F32X3), cn[0].f32_first = 0, cn[0].f32_count = 3;
  cn[1].op = uint32_t(ar::Op::CONST_F32), cn[1].f32_first = 3, cn[1].f32_count = 1;
  cn[2].op = uint32_t(ar::Op::EMIT_LE);
  const ar_graph_link cl[2] = {{0, 0, 2, 0}, {1, 0, 2, 1}};
  ar_graph_desc cd = {};
  cd.node_count = 3, cd.link_count = 2, cd.f32_count = 4, cd.nodes = cn, cd.links = cl, cd.params_f32 = cf;
  commit([&](Edit& e) { REQUIRE(store.set_graph(e, 40, &cd) == AR_OK); });
  CHECK(store.world()->program.empty());
  CHECK(store.arrays()->world_program.empty());
  CHECK(store.arrays()->world_constants.empty());
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
