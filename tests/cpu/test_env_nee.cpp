// SPDX-License-Identifier: Apache-2.0
// Environment NEE and MIS (0004 D11, Verification row D7/D11; ruling 88): the WORLD_ENV light samples
// the MIS-compensated table and reports the compensated pdf, which pdf_li gives back at the sampled
// direction (exactly away from cell edges, a neighbour's pdf within the mapping's round-trip error of
// one); the light table gives a baked program world the importance 4 pi mean x strength x pi R^2 and
// the sampler's pmf of a sampled light is its pmf; 1e5 draws follow the compensated function
// (chi-squared, gate); and through the stages (shade_surface's NEE item, shade_shadow's delivery of
// the world program's radiance, shade_background's escaped ray weighted with the same compensated
// pdf) a white Lambertian plane under a uniform program world of radiance 0.5 reflects 0.5, and
// under a world peaked toward +z, whose compensated and plain tables differ, with a record strength
// != 1, reflects its quadrature (gate).
// Ref: docs/decisions/0004-integrator.md D1, D7, D10, D11 and Verification D7/D11 (ruling 88's rule);
// third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/lights.h ImageInfiniteLight::SampleLi and
// lights.cpp PDF_Li, Phi (Apache-2.0); Karlik et al. 2019 (MIS compensation); tests/cpu/test_env_table.cpp
// (the round-trip error e and the near-edge bound of ruling 88, derived there).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "common/stat.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/film/film_layout.h"
#include "host/light/env_bake.h"
#include "host/light/env_table.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_background.h"
#include "kernel/stages/shade_shadow.h"
#include "kernel/stages/shade_surface.h"
#include "vm/ops.h"

namespace st = astroray::stat;
using namespace astroray;
using namespace astroray::host;
using ar::Real;
using ar::Real2;
using ar::Real3;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kN = 64;

// The n = 64 grid of test_env_table.cpp: a smooth positive field, one bright cell, a band of zero
// rows and scattered zero cells, so the compensated function differs from the plain one everywhere.
std::vector<double> test_grid() {
  std::vector<double> g(size_t(kN) * kN);
  for (uint32_t v = 0; v < kN; ++v)
    for (uint32_t u = 0; u < kN; ++u) {
      double x = 1.0 + 0.5 * std::sin(0.3 * u) * std::cos(0.2 * v);
      if (v >= 20 && v < 24) x = 0.0;
      if ((u * 7 + v * 13) % 17 == 0) x = 0.0;
      g[size_t(v) * kN + u] = x;
    }
  g[size_t(40) * kN + 9] = 2000.0;
  return g;
}

// The compensated function written from pbrt's lines (lights.cpp ImageInfiniteLight), independent of
// the builder: max(L - mean, 0), all ones when that is 0 everywhere.
std::vector<double> compensated_of(const std::vector<double>& g) {
  double sum = 0.0;
  for (double v : g) sum += v;
  const double avg = sum / double(g.size());
  std::vector<double> c(g.size());
  bool all_zero = true;
  for (size_t i = 0; i < g.size(); ++i) {
    c[i] = std::max(g[i] - avg, 0.0);
    all_zero = all_zero && c[i] == 0.0;
  }
  if (all_zero) std::fill(c.begin(), c.end(), 1.0);
  return c;
}

ar::EnvTableView view_of(const EnvDistribution& d) {
  return ar::EnvTableView{d.func.data(), d.cdf_u.data(), d.marginal_func.data(), d.marginal_cdf.data(), d.func_integral, d.n};
}

// A WORLD_ENV record as the light table makes it from a program world (host build_light_table).
ar::LightRecord env_record(float strength) {
  ar::LightRecord r = ar::light_make_world(Real(strength), ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)),
                                           ar::kLightWorldProgram);
  r.type = uint8_t(ar::LightType::WORLD_ENV);
  return r;
}

ar::ObserverView flat_observer() {
  static const std::vector<float> cmf(3 * ar::kObsSamples, 1.0f), illum(ar::kObsSamples, 1.0f);
  return ar::ObserverView{cmf.data(), illum.data()};
}

ar::Lambda lanes() {
  ar::Lambda lam;
  for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = Real(450 + 50 * k);
  return lam;
}

// Max per-coordinate error of the kernel's square -> sphere -> square round trip over 1e5 uniform
// points (fixed seed): ruling 88's e, measured as test_env_table.cpp measures it.
double square_round_trip_error() {
  std::mt19937_64 rng(153);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  double e = 0.0;
  for (int k = 0; k < 100000; ++k) {
    const Real2 p{Real(uni(rng)), Real(uni(rng))};
    const Real2 q = ar::equal_area_sphere_to_square(ar::equal_area_square_to_sphere(p));
    e = std::max(e, std::max(std::fabs(double(q.x) - double(p.x)), std::fabs(double(q.y) - double(p.y))));
  }
  return e;
}

bool rel_close(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(std::fabs(a), std::fabs(b)); }

// ---- the stage rig -------------------------------------------------------------------------------

struct Graph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(ar::Op op, std::vector<float> fp, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  int op2(ar::Op op, int a, int b) {
    const int n = add(op, {});
    links.push_back({uint32_t(a), 0u, uint32_t(n), 0u});
    links.push_back({uint32_t(b), 0u, uint32_t(n), 1u});
    return n;
  }
  // Emission of (1, 1, 1) times kFloor plus w^8, w = (1 + z) / 2 of the shading position: in a world
  // program the position is the escaping direction, so the world is brightest toward +z.
  void emit_peaked() {
    emit({1.0f, 1.0f, 1.0f}, kFloor);
    const int z = add(ar::Op::CHANNEL_F32X3, {}, {2});
    links.push_back({uint32_t(add(ar::Op::LOAD_CTX_P, {})), 0u, uint32_t(z), 0u});
    const int w = op2(ar::Op::ADD_F32, op2(ar::Op::MUL_F32, z, add(ar::Op::CONST_F32, {0.5f})), add(ar::Op::CONST_F32, {0.5f}));
    const int w2 = op2(ar::Op::MUL_F32, w, w), w4 = op2(ar::Op::MUL_F32, w2, w2), w8 = op2(ar::Op::MUL_F32, w4, w4);
    op2(ar::Op::EMIT_LE, add(ar::Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), w8);
  }
  static constexpr float kFloor = 0.1f;
  void emit(std::vector<float> rgb, float strength) {
    const int c = add(ar::Op::CONST_F32X3, rgb), s = add(ar::Op::CONST_F32, {strength});
    const int e = add(ar::Op::EMIT_LE, {});
    links.push_back({uint32_t(c), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(s), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

constexpr ar_key kMesh = 100, kWorldGraph = 40;

// A quad of half-size 1 at z = 0 facing +z (the default material, made a white Lambertian below) under
// a world of two white Emissions of 0.25 each: radiance 0.5 from every direction, but a program (two
// emission terms do not fold into the constant block), so the scene table build bakes it and the light
// table makes it WORLD_ENV. Every slot hits the quad's first triangle from above with throughput 1 and
// r_u 1; nothing else is in the scene, so every shadow item is unoccluded and every continuation
// escapes. `peaked`: the world is Graph::emit_peaked's instead, whose compensated table differs from
// the plain one, and the world record's strength is 3, as a radiance scale s != 1 makes it (gate
// builds pin s to 1), so NEE must carry the strength as the escaped ray does.
struct Rig {
  ContextState sctx;
  Scene scene{sctx, SceneConfig{2, {}, {}}};
  std::shared_ptr<const SceneTableSet> set;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::SceneData sd{};
  std::vector<ar::ConstantBlock> blocks;
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  std::vector<uint32_t> queue, bg_queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<Real3> ray_o, ray_d;
  std::vector<Real2> hit_uv;
  std::vector<Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;
  std::vector<uint32_t> sh_identity, sh_queue, sh_target;
  std::vector<uint8_t> sh_next, sh_flags;
  std::vector<Real3> sh_o, sh_d;
  std::vector<Real> sh_tmax;
  std::vector<ar::Spec> sh_contrib, sh_lambda;
  std::vector<ar::LightRecord> lights;
  uint32_t n;

  explicit Rig(uint32_t slots, bool peaked = false) : n(slots) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    // Uniform: the table is uniform at any size. Peaked: any table is unbiased; 32^2 cells resolve
    // the peak, so the compensated table is non-zero only toward +z.
    scene.table_cache().bake_size = peaked ? 32 : 16;
    commit([&](Edit& e) {
      Graph g;
      if (peaked) {
        g.emit_peaked();
      } else {
        g.emit({1.0f, 1.0f, 1.0f}, 0.25f);
        g.emit({1.0f, 1.0f, 1.0f}, 0.25f);
      }
      const ar_graph_desc d = g.desc();
      REQUIRE(scene.materials->set_graph(e, kWorldGraph, &d) == AR_OK);
      ar_world_desc w = {};
      w.surface_graph = kWorldGraph;
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0}, uv{0, 0, 1, 0, 1, 1, 0, 1};
      const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
      ar_mesh_desc md = {};
      md.key = kMesh, md.vertex_count = 4, md.corner_count = 4, md.face_count = 1, md.motion_steps = 1, md.uv_layer_count = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &md, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      std::memcpy(*b.uv, uv.data(), uv.size() * 4);
      REQUIRE(e.item_commit(kMesh) == AR_OK);
    });
    commit([&](Edit& e) {
      std::vector<ar_key> geom{kMesh}, object{500};
      std::vector<ar_xform> xf(1);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff(1, 0), mcount{0};
      std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(1, 0);
      ar_instances_desc d = {};
      d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 0;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = nullptr;
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);
    REQUIRE(set->lights);
    REQUIRE(set->lights->world_light != ar::kNoLight);
    REQUIRE(set->lights->lights[set->lights->world_light].type == uint8_t(ar::LightType::WORLD_ENV));

    FilmRequest req;
    req.max_width = n;
    req.max_height = 1;
    req.slot_capacity = n;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0 / set->lights->radiance_scale;

    queue.resize(n), active.resize(n), sample_id.resize(n), flags.resize(n), hit_prim.assign(n, 0u), hit_inst.assign(n, 0u);
    prev_ng.assign(n, 0u), prev_ns.assign(n, 0u), next_stage.resize(n), term.resize(n), ray_type.assign(n, 1u);
    event_index.resize(n), bounces.resize(n), ray_o.resize(n), ray_d.resize(n), hit_uv.resize(n), hit_t.assign(n, Real(1));
    eta_scale.assign(n, Real(1)), prev_dist.assign(n, Real(0)), lambda_t.resize(n), beta.resize(n), r_u.resize(n), r_l.resize(n);
    sh_identity.resize(n), sh_target.resize(n), sh_next.resize(n), sh_flags.resize(n), sh_o.resize(n), sh_d.resize(n);
    sh_tmax.resize(n), sh_contrib.resize(n), sh_lambda.resize(n);
    for (uint32_t i = 0; i < n; ++i) queue[i] = active[i] = sample_id[i] = sh_identity[i] = i;
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();
    shadow.next_stage = sh_next.data(), shadow.ray_o = sh_o.data(), shadow.ray_d = sh_d.data();
    shadow.tmax = sh_tmax.data(), shadow.contribution = sh_contrib.data(), shadow.target = sh_target.data();
    shadow.item_flags = sh_flags.data(), shadow.lambda_t = sh_lambda.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 17;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    kg = build_kernel_globals(set.get(), in);
    REQUIRE(kg.session.features.env_world);
    kg.path = &path;
    kg.shadow = &shadow;
    if (peaked) {
      lights = set->lights->lights;
      lights[set->lights->world_light].strength *= 3.0f;
      kg.lights = lights.data();
    }
    // The default material, made a white Lambertian (base colour 1, no specular, no emission).
    sd = *kg.scene;
    blocks.assign(sd.material_constants, sd.material_constants + kg.material_constant_count);
    sd.material_constants = blocks.data();
    kg.scene = &sd;
    ar::ConstantBlock& bl = blocks[sd.material_headers[sd.material_header_count].const_offset];
    ar::SpectrumRef one{};
    one.kind = uint8_t(ar::SpectrumKind::CONSTANT);
    one.p[0] = 1.0f;
    bl.base_weight = 1.0f, bl.base_color = one, bl.base_diffuse_roughness = 0.0f, bl.base_metalness = 0.0f;
    bl.specular_weight = 0.0f, bl.emission_luminance = 0.0f;
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  // One pass: every slot hits the quad, shade_surface runs, its escaping continuations go to
  // shade_background and its shadow items to shade_shadow (nothing can occlude them). Returns the
  // film slots' radiance, 4 lanes per sample.
  std::vector<float> pass(uint32_t index, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    ar::FilmData& fd = film->data();
    std::fill(fd.slot_L, fd.slot_L + size_t(n) * 4, 0.0f);
    std::fill(fd.slot_flags, fd.slot_flags + n, 0u);
    kg.session.samples_done = index;
    for (uint32_t i = 0; i < n; ++i) {
      next_stage[i] = uint8_t(ar::Stage::SHADE_SURFACE), term[i] = uint8_t(ar::Term::ACTIVE);
      flags[i] = ar::set_hero_lane(0u, i & 3u), event_index[i] = 0, bounces[i] = ar::U16x6{{0, 0, 0, 0, 0, 0}};
      double u = uni(rng), v = uni(rng);
      if (u + v > 1.0) u = 1.0 - u, v = 1.0 - v;
      hit_uv[i] = Real2{Real(u), Real(v)};
      // The point of barycentrics (u, v) on the first triangle (-1,-1,0), (1,-1,0), (1,1,0).
      const Real3 p{Real(-1 + 2 * (u + v)), Real(-1 + 2 * v), Real(0)};
      ray_o[i] = Real3{p.x * Real(0.5), p.y * Real(0.5), Real(1)};
      ray_d[i] = ar::normalize(p - ray_o[i]);
      for (int l = 0; l < ar::kLanes; ++l) lambda_t[i].v[l] = Real(380.0 + 400.0 * uni(rng));
      beta[i] = r_u[i] = r_l[i] = ar::spec_fill(Real(1));
      prev_dist[i] = Real(0);
      sh_next[i] = uint8_t(ar::Stage::SHADOW_TERMINATED), sh_target[i] = ~0u, sh_flags[i] = 0xFF;
    }
    kg.main_queue = queue.data();
    kg.shadow_queue = sh_identity.data();
    cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n);
    bg_queue.clear(), sh_queue.clear();
    for (uint32_t i = 0; i < n; ++i) {
      if (next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST)) {
        next_stage[i] = uint8_t(ar::Stage::SHADE_BACKGROUND);
        bg_queue.push_back(i);
      }
      if (sh_next[i] == uint8_t(ar::Stage::INTERSECT_SHADOW)) sh_queue.push_back(i);
    }
    const uint32_t nb = uint32_t(bg_queue.size()), ns = uint32_t(sh_queue.size());
    kg.main_queue = bg_queue.data();
    cpu_stage_shade_background(kg, ar::StepEntry{0, nb, 0}, 0, nb);
    kg.shadow_queue = sh_queue.data();
    cpu_stage_shade_shadow(kg, ar::StepEntry{0, ns, 0}, 0, ns);
    for (uint32_t i : sh_queue) CHECK(sh_flags[i] == ar::ITEM_WORLD_LE);  // every item is toward the world
    return std::vector<float>(fd.slot_L, fd.slot_L + size_t(n) * 4);
  }
  // The world's radiance toward +z at the lanes of slot i, times the record's strength (which
  // carries s), as shade_background adds it for an escaped ray (the uniform world's is the same in
  // every direction).
  ar::Spec world_le(uint32_t i) const {
    ar::Lambda lam;
    for (int l = 0; l < ar::kLanes; ++l) lam.nm[l] = lambda_t[i].v[l];
    ar::Spec e = ar::spec_fill(Real(0));
    REQUIRE(ar::world_program_radiance(*kg.scene, *kg.tables, ar::world_shade_context(Real3{0, 0, 1}, lam), &e));
    return e * Real(kg.lights[set->lights->world_light].strength);
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("env_nee/pdf matches sample") {
  const EnvTable t = build_env_table(test_grid(), kN);
  const ar::EnvTableView env[2] = {view_of(t.plain), view_of(t.compensated)};
  const ar::LightRecord rec = env_record(1.0f);
  REQUIRE(ar::light_problem(rec) == nullptr);
  CHECK(ar::light_is_infinite(rec));
  ar::LightRecord wrong = rec;
  wrong.flags = 0;
  CHECK(ar::light_problem(wrong) != nullptr);  // a WORLD_ENV record without its program is refused
  const ar::LightSampleContext ctx = ar::light_context_point(Real3{0.2f, -0.3f, 0.1f}, Real3{0, 0, 1}, Real3{0, 0, 1});
  const ar::ObserverView obs = flat_observer();
  const EnvDistribution& dist = t.compensated;
  const uint32_t n = dist.n;
  const double e_uniform = square_round_trip_error();
  const int draws = 100000;
  std::mt19937_64 rng(1733);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  struct Draw {
    ar::LightLiSample s;
    ar::EnvDirSample ref;
  };
  std::vector<Draw> ds(draws);
  double e = e_uniform;
  int same = 0;
  for (Draw& d : ds) {
    const Real2 u{Real(uni(rng)), Real(uni(rng))};
    d.s = ar::light_sample_li(rec, ctx, u, lanes(), obs, env);
    // The sample is the compensated table's (allow_incomplete), never the plain one's.
    d.ref = ar::env_sample(env[0], env[1], u, true);
    same += d.s.pdf == d.ref.pdf && d.s.wi.x == d.ref.wi.x && d.s.wi.y == d.ref.wi.y && d.s.wi.z == d.ref.wi.z &&
            d.s.infinite && d.s.deferred_le && !d.s.delta && ar::max_lane(d.s.Le) == Real(0);
    const Real2 q = ar::equal_area_sphere_to_square(d.s.wi);
    e = std::max({e, std::fabs(double(q.x) - double(d.ref.p.x)), std::fabs(double(q.y) - double(d.ref.p.y))});
  }
  CHECK(same == draws);
  CHECK(e <= 1e-4);  // test_env_table.cpp's float bound, poles included
  const auto cell_pdf = [&](uint32_t iu, uint32_t iv) {
    return double(dist.func[size_t(iv) * n + iu]) / double(dist.func_integral) / (4.0 * kPi);
  };
  const auto edge = [&](double c, int* step) {
    const double k = std::round(c * n);
    if (k < 1.0 || k > double(n) - 1.0) return 2.0;
    *step = c < k / n ? 1 : -1;
    return std::fabs(c - k / n);
  };
  int near = 0, bad_far = 0, bad_near = 0, bad_cell = 0;
  for (const Draw& d : ds) {
    const double pdf = double(ar::light_pdf_li(rec, ctx, d.s.wi, ar::LightHit{}, env));
    const uint32_t iu = d.ref.cell % n, iv = d.ref.cell / n;
    bad_cell += !rel_close(double(d.s.pdf), cell_pdf(iu, iv), 1e-4);  // the compensated cell's pdf
    int sx = 0, sy = 0;
    const double dx = edge(double(d.ref.p.x), &sx), dy = edge(double(d.ref.p.y), &sy);
    if (std::min(dx, dy) > e) {
      bad_far += !rel_close(pdf, double(d.s.pdf), 1e-4);
      continue;
    }
    ++near;
    bool ok = rel_close(pdf, double(d.s.pdf), 1e-4);
    if (dx <= e) ok = ok || rel_close(pdf, cell_pdf(iu + sx, iv), 1e-4);
    if (dy <= e) ok = ok || rel_close(pdf, cell_pdf(iu, iv + sy), 1e-4);
    if (dx <= e && dy <= e) ok = ok || rel_close(pdf, cell_pdf(iu + sx, iv + sy), 1e-4);
    bad_near += !ok;
  }
  // Ruling 88's bound (test_env_table.cpp's header): the cell-probability-weighted band of width 2e
  // around the interior edges, plus 3 binomial SE.
  double band = 0.0;
  for (uint32_t iv = 0; iv < n; ++iv)
    for (uint32_t iu = 0; iu < n; ++iu) {
      const double fx = std::min(1.0, ((iu > 0) + (iu + 1 < n)) * e * n), fy = std::min(1.0, ((iv > 0) + (iv + 1 < n)) * e * n);
      band += double(dist.func[size_t(iv) * n + iu]) / double(dist.func_integral) / (double(n) * n) * (fx + fy - fx * fy);
    }
  const double bound = band + 3.0 * std::sqrt(band * (1.0 - band) / draws), frac = double(near) / draws;
  std::printf("env_nee pdf_li: e %.3e, near-edge fraction %.3e, bound %.3e\n", e, frac, bound);
  CHECK(bad_cell == 0);
  CHECK(bad_far == 0);
  CHECK(bad_near == 0);
  CHECK(frac <= bound);
  // No table, no sample; sample_le of an environment world is M4 and not called here.
  const ar::EnvTableView none[2] = {};
  CHECK(ar::light_sample_li(rec, ctx, Real2{Real(0.3), Real(0.6)}, lanes(), obs, none).pdf == Real(0));
}

TEST_CASE("env_nee/pmf matches sample") {
  // A point lamp and a program world: with the world's bake, the world is WORLD_ENV and its importance
  // is 4 pi mean x strength (its w-weighted radiance integrated over the sphere) times pi R^2 (pbrt
  // ImageInfiniteLight::Phi = 4 pi^2 R^2 mean); every sampled light's pmf(ctx, light) is the pmf the
  // sample reported (0004 Verification D7/D11, T0).
  const ar::ObserverView obs = flat_observer();
  const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1));
  const ar::SceneSphere sphere{Real3{0, 0, 0}, Real(10)};
  LightInput point, world;
  point.record = ar::light_make(ar::LightType::POINT, Real3{0, 0, 1}, Real(0), Real(500), c);
  world.record = ar::light_make_world(Real(1.5), c, ar::kLightWorldProgram);
  BakedEnvironment baked;
  baked.table = std::make_shared<const EnvTable>(build_env_table(test_grid(), kN));
  baked.max = 2.0, baked.mean = 0.75;
  const auto t = build_light_table({point, world}, sphere, obs, nullptr, {}, &baked);
  REQUIRE(t->world_light == 1u);
  CHECK(t->lights[1].type == uint8_t(ar::LightType::WORLD_ENV));
  CHECK(ar::light_problem(t->lights[1]) == nullptr);
  const double world_imp = 4.0 * kPi * 0.75 * 1.5 * kPi * 10.0 * 10.0;
  CHECK(t->importance[1] == doctest::Approx(world_imp).epsilon(1e-6));
  CHECK(t->importance[0] == doctest::Approx(500.0).epsilon(1e-5));  // the flat observer's display energy is 1
  CHECK(double(t->pmf[1]) == doctest::Approx(world_imp / (world_imp + 500.0)).epsilon(1e-6));
  const ar::LightSamplerView v = t->view();
  const ar::LightSampleContext ctx{};
  uint32_t worlds = 0;
  const uint32_t draws = 100000;
  int bad = 0;
  for (uint32_t k = 0; k < draws; ++k) {
    const ar::SampledLight s = ar::light_sampler_sample(v, ctx, uint32_t((uint64_t(k) << 32) / draws), Real(((k * 7919u) % 1000u + 0.5) / 1000.0));
    REQUIRE(s.light < 2u);
    bad += ar::light_sampler_pmf(v, ctx, s.light) != s.pmf;
    worlds += s.light == 1u;
  }
  CHECK(bad == 0);
  // The stratified draws pick the world in proportion to its pmf to well under a percent.
  CHECK(double(worlds) / draws == doctest::Approx(double(t->pmf[1])).epsilon(1e-2));
  // A black program world (no table) or no bake: WORLD_CONST, importance 0, never sampled.
  BakedEnvironment black;
  const auto tb = build_light_table({point, world}, sphere, obs, nullptr, {}, &black);
  CHECK(tb->lights[1].type == uint8_t(ar::LightType::WORLD_CONST));
  CHECK(tb->importance[1] == 0.0);
  CHECK(build_light_table({point, world}, sphere, obs, nullptr)->pmf[1] == 0.0f);
  // A constant world stays WORLD_CONST with its own energy whatever the bake.
  world.record = ar::light_make_world(Real(1.5), c);
  const auto tc = build_light_table({point, world}, sphere, obs, nullptr, {}, &baked);
  CHECK(tc->lights[1].type == uint8_t(ar::LightType::WORLD_CONST));
  CHECK(tc->importance[1] == doctest::Approx(1.5 * 4.0 * kPi * kPi * 100.0).epsilon(1e-5));
}

}  // TEST_SUITE("fast")

TEST_SUITE("gate") {

TEST_CASE("env_nee/chi squared against table") {
  // 1e5 NEE directions of a WORLD_ENV light against the compensated function written from pbrt's
  // lines, in 16 x 16 bins of an n = 64 table. Each draw is the compensated env_sample's of the same
  // uniforms (direction bit-equal, checked), and is binned by that sample's square point: the
  // direction's own round trip may move it across a cell edge into a zero bin (ruling 88), which says
  // nothing about the distribution. The field varies smoothly from 0.1 to 1.9 with one cell of 30, so
  // the compensated function max(L - mean, 0) holds mass in about half the cells.
  const st::Run run = st::begin("env_nee/chi squared against table", st::gate_family());
  std::vector<double> g(size_t(kN) * kN);
  for (uint32_t v = 0; v < kN; ++v)
    for (uint32_t u = 0; u < kN; ++u) g[size_t(v) * kN + u] = 1.0 + 0.9 * std::sin(0.3 * u) * std::cos(0.2 * v);
  g[size_t(40) * kN + 9] = 30.0;
  const std::vector<double> f = compensated_of(g);
  const EnvTable t = build_env_table(g, kN);
  const ar::EnvTableView env[2] = {view_of(t.plain), view_of(t.compensated)};
  const ar::LightRecord rec = env_record(1.0f);
  const ar::LightSampleContext ctx = ar::light_context_point(Real3{0, 0, 0}, Real3{0, 0, 1}, Real3{0, 0, 1});
  const int bins = 16, per = int(kN) / bins, samples = 100000;
  std::vector<double> observed(size_t(bins) * bins, 0.0), expected(size_t(bins) * bins, 0.0);
  double total = 0.0;
  for (double x : f) total += x;
  for (uint32_t iv = 0; iv < kN; ++iv)
    for (uint32_t iu = 0; iu < kN; ++iu) expected[size_t(iv / per) * bins + iu / per] += samples * f[size_t(iv) * kN + iu] / total;
  std::mt19937_64 rng(run.seed);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  int differ = 0;
  for (int k = 0; k < samples; ++k) {
    const Real2 u{Real(uni(rng)), Real(uni(rng))};
    const ar::LightLiSample s = ar::light_sample_li(rec, ctx, u, lanes(), flat_observer(), env);
    const ar::EnvDirSample q = ar::env_sample(env[0], env[1], u, true);
    REQUIRE(s.pdf > Real(0));
    differ += s.wi.x != q.wi.x || s.wi.y != q.wi.y || s.wi.z != q.wi.z;
    const int bx = std::min(bins - 1, int(double(q.p.x) * bins)), by = std::min(bins - 1, int(double(q.p.y) * bins));
    observed[size_t(by) * bins + bx] += 1.0;
  }
  CHECK(differ == 0);
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  std::printf("env_nee chi2: chi2 %.1f dof %d p %.4f\n", res.chi2, res.dof, res.p);
  CHECK(res.dof >= 50);
  CHECK_MESSAGE(res.pass, res.message);
}

TEST_CASE("env_nee/constant world half") {
  // A white Lambertian plane under a uniform program world of radiance 0.5 reflects 0.5 (0004
  // Verification D7/D11): per sample, the film slot (NEE delivered through shade_shadow plus the
  // escaped BSDF ray through shade_background, combined by the balance heuristic) over the world's
  // radiance at the sample's lanes has mean 1.
  const st::Run run = st::begin("env_nee/constant world half", st::gate_family());
  Rig rig(1024);
  std::mt19937_64 rng(run.seed);
  std::vector<double> ratio;
  double world_half = 0.0;
  for (uint32_t p = 0; p < 16; ++p) {
    const std::vector<float> slots = rig.pass(p, rng);
    for (uint32_t i = 0; i < rig.n; ++i) {
      const ar::Spec le = rig.world_le(i);
      double r = 0.0;
      for (int l = 0; l < ar::kLanes; ++l) r += double(slots[4u * rig.sample_id[i] + uint32_t(l)]) / double(le.v[l]);
      ratio.push_back(r / ar::kLanes);
      if (p == 0 && i == 0) {
        for (int l = 0; l < ar::kLanes; ++l) world_half += double(le.v[l]);
        world_half /= ar::kLanes * rig.set->lights->radiance_scale;
      }
    }
  }
  const st::EnergyResult res = st::energy_test(run, ratio, 1.0);
  std::printf("env_nee constant world: reflected / world radiance %.5f +- %.5f (z %.2f, %zu samples; world radiance at lane mean %.4f)\n",
              res.mean, res.se, res.z, ratio.size(), world_half);
  CHECK_MESSAGE(res.pass, res.message);
  CHECK(std::fabs(res.mean - 1.0) <= 3.0 * res.se);
}

TEST_CASE("env_nee/peaked world reflects its integral") {
  // A white Lambertian plane facing +z under the world kFloor + ((1 + z) / 2)^8 (Graph::emit_peaked):
  // its compensated table holds mass only toward +z and its plain one everywhere, so NEE (compensated)
  // and the escaped ray's weight must read the same table or the dim directions are counted wrongly.
  // Reflected radiance = (1 / pi) integral of L cos over the upper hemisphere = kFloor + 2 I, with
  // I = integral over z in [0, 1] of ((1 + z) / 2)^8 z dz (dw = dphi dz), by midpoint quadrature; per
  // sample, film slot over the world's radiance toward +z (1 + kFloor) has mean (kFloor + 2 I) /
  // (1 + kFloor). The world record's strength is 3, so NEE without it reads low.
  // Ref: Lambert's cosine law; Veach 1997 ch. 9 (the balance heuristic is unbiased only when both
  // strategies' weights use the same two densities); docs/decisions/0004-integrator.md D11.
  const st::Run run = st::begin("env_nee/peaked world reflects its integral", st::gate_family());
  Rig rig(1024, true);
  REQUIRE(rig.set->lights->env);
  const EnvDistribution& pl = rig.set->lights->env->plain;
  const EnvDistribution& co = rig.set->lights->env->compensated;
  size_t differ = 0;
  for (size_t k = 0; k < pl.func.size(); ++k) differ += (pl.func[k] / pl.func_integral) != (co.func[k] / co.func_integral);
  CHECK(differ > pl.func.size() / 2);  // the two tables differ in most cells
  double integral = 0.0;
  const int q = 100000;
  for (int k = 0; k < q; ++k) {
    const double z = (k + 0.5) / q, w = 0.5 * (1.0 + z);
    integral += std::pow(w, 8) * z / q;
  }
  const double floor_l = double(Graph::kFloor), truth = (floor_l + 2.0 * integral) / (1.0 + floor_l);
  std::mt19937_64 rng(run.seed);
  std::vector<double> ratio;
  for (uint32_t p = 0; p < 16; ++p) {
    const std::vector<float> slots = rig.pass(p, rng);
    for (uint32_t i = 0; i < rig.n; ++i) {
      const ar::Spec le = rig.world_le(i);
      double r = 0.0;
      for (int l = 0; l < ar::kLanes; ++l) r += double(slots[4u * rig.sample_id[i] + uint32_t(l)]) / double(le.v[l]);
      ratio.push_back(r / ar::kLanes);
    }
  }
  const st::EnergyResult res = st::energy_test(run, ratio, truth);
  std::printf("env_nee peaked world: reflected / radiance toward +z %.5f +- %.5f, truth %.5f (z %.2f, %zu samples)\n", res.mean,
              res.se, truth, res.z, ratio.size());
  CHECK_MESSAGE(res.pass, res.message);
}

}  // TEST_SUITE("gate")
