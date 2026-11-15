// SPDX-License-Identifier: Apache-2.0
// NEE, shadow items, delivery and emitter-hit MIS (task 074). Hand-built hits on a real scene's
// tables: a point lamp gives the item beta f |cos| I / d^2 / (pmf avg(r_u)) (delta light, weight
// 1), a rectangle lamp the balance weight against the BSDF pdf, each in the path's reserved
// shadow slot with the sample's id as target and an offset segment ending at the lamp; a lamp
// below the surface emits no item. A program-emission triangle light (deferred Le) gives the same
// item as an equal constant emitter, so its emission entry is evaluated, times s, once.
// shade_shadow adds a delivered item to its target slot after the path ended and its pool slot
// was refilled. A miss after a diffuse bounce carries the balance weight against world NEE.
// Ref: docs/decisions/0004-integrator.md D1, D4, D5, D9; 0002 D5, D16; 0003 d10, d20, d23;
// pbrt-v4 VolPathIntegrator::SampleLd and Li (Apache-2.0); integrator/mis.h.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/film/film_layout.h"
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

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr ar_key kMesh = 100, kEmit = 1;
constexpr uint32_t kSlots = 4;

struct Graph {
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
  void emit(std::vector<float> rgb, float strength) {
    const int c = add(ar::Op::CONST_F32X3, rgb), s = add(ar::Op::CONST_F32, {strength});
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

// What lights the receiver: hand-built lamps replace the scene's table, or the scene holds an
// emitter quad at z = 1 facing down whose radiance 4 is one constant Emission (folded into the
// constant block) or two Emissions of 2 (the program's emission entry: deferred Le).
enum class Emitter { NONE, CONSTANT, PROGRAM };

// A quad of half-size 1 at z = 0 facing +z (instance 0, the default material) receives; slot i
// hits it at a point of its first triangle with sample id kSlots - 1 - i, and owns shadow slot
// kSlots - 1 - i, so neither the film slot nor the shadow slot is the pool slot.
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
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;
  std::vector<uint32_t> sh_queue, sh_target;
  std::vector<uint8_t> sh_next;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<ar::Real> sh_tmax;
  std::vector<ar::Spec> sh_contrib;
  // Hand-built light table.
  std::vector<ar::LightRecord> lamps;
  std::vector<ar::AliasBin> bins;
  std::vector<uint32_t> bin_light;
  std::vector<float> pmf;
  ar::LightSamplerView sampler{};

  uint32_t n;

  explicit Rig(Emitter emitter, uint32_t slots = kSlots) : n(slots) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      if (emitter != Emitter::NONE) {
        Graph g;
        if (emitter == Emitter::CONSTANT) g.emit({1.0f, 0.5f, 0.25f}, 4.0f);
        else g.emit({1.0f, 0.5f, 0.25f}, 2.0f), g.emit({1.0f, 0.5f, 0.25f}, 2.0f);
        const ar_graph_desc d = g.desc();
        REQUIRE(scene.materials->set_graph(e, kEmit + 1000, &d) == AR_OK);
        ar_material_desc m = {};
        m.surface_graph = kEmit + 1000;
        m.emission_sampling = AR_EMISSION_SAMPLING_FRONT;
        REQUIRE(scene.materials->set_material(e, kEmit, &m) == AR_OK);
      }
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
      const uint32_t n = emitter == Emitter::NONE ? 1u : 2u;
      std::vector<ar_key> geom(n, kMesh), object{500, 501}, keys{kEmit};
      std::vector<ar_xform> xf(n);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      if (n == 2) xf[1].m[0] = 1.0f, xf[1].m[5] = xf[1].m[10] = -1.0f, xf[1].m[11] = 1.0f;  // flipped, at z = 1
      std::vector<uint32_t> zeros(n, 0), vis(n, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff(n, 0), mcount{0, 1};
      std::vector<ar_rgba> colors(n, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(n, 0);
      ar_instances_desc d = {};
      d.instance_count = n, d.motion_steps = 1, d.transform_count = n, d.material_key_count = n - 1;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);

    FilmRequest req;
    req.max_width = n;
    req.max_height = 1;
    req.slot_capacity = n;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0 / set->lights->radiance_scale;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(n - 1 - i);
      flags.push_back(ar::set_hero_lane(0u, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u), event_index.push_back(uint16_t(i % 4));
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{ar::Real(0.1 + 0.15 * (i % 4)), ar::Real(0.2 + 0.1 * (i % 4))});
      hit_t.push_back(ar::Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1});
      ray_d.push_back(ar::Real3{ar::Real(0.1), ar::Real(-0.2), ar::Real(-1)});
      eta_scale.push_back(ar::Real(1));
      prev_ng.push_back(0u), prev_ns.push_back(0u), prev_dist.push_back(ar::Real(0));
      ar::Spec lt, b, ru;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = ar::Real(430 + 70 * l);
        b.v[l] = ar::Real(0.5 + 0.125 * double(l));
        ru.v[l] = ar::Real(1.0 + 0.25 * double(l));  // r_u not 1, so a missing r_u factor shows
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(ru);
      sh_queue.push_back(n - 1 - i), sh_target.push_back(~0u);
      sh_next.push_back(uint8_t(ar::Stage::SHADOW_TERMINATED));
      sh_o.push_back(ar::Real3{0, 0, 0}), sh_d.push_back(ar::Real3{0, 0, 0}), sh_tmax.push_back(ar::Real(-1));
      sh_contrib.push_back(ar::spec_fill(ar::Real(0)));
    }
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

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 5;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.shadow = &shadow;
    kg.main_queue = queue.data();
    kg.shadow_queue = sh_queue.data();
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
  // Replaces the scene's light table by `lights` with selection probabilities `p` (an alias table
  // with one bin per light: bin i keeps light i with probability q_i = n p_i, else gives the light
  // with the largest p, which holds when that light's own bin is full; fine for the 1-2 lights here).
  void use_lamps(const std::vector<ar::LightRecord>& lights, const std::vector<float>& p) {
    lamps = lights, pmf = p;
    const uint32_t nl = uint32_t(p.size());
    uint32_t big = 0;
    for (uint32_t i = 0; i < nl; ++i) big = p[i] > p[big] ? i : big;
    bins.clear(), bin_light.clear();
    for (uint32_t i = 0; i < nl; ++i) bins.push_back({i == big ? 1.0f : float(nl) * p[i], big}), bin_light.push_back(i);
    sampler = ar::LightSamplerView{bins.data(), bin_light.data(), pmf.data(), nl, nl};
    kg.lights = lamps.data();
    kg.light_sampler = &sampler;
  }
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  // The hit context and record of pool slot s, as the stage builds them before it runs.
  void vertex(uint32_t s, ar::ShadeContext* c, ar::OpenPBRParams* rec) {
    REQUIRE(ar::shade_context_build(kg, s, c));
    const ar::SceneData& sd = *kg.scene;
    const ar::MaterialHeader& h = sd.material_headers[sd.material_header_count];  // the default material
    ar::resolve_constant_record(sd.material_constants[h.const_offset], *c, kg.tables->observer, rec);
  }
  uint32_t item_of(uint32_t s) const { return sh_queue[s]; }
};

// A double replica of Arvo's spherical-triangle map in pbrt-v4 SampleSphericalTriangle's form: the
// reference the float sampler (Peters' steps, triangle.h) is measured against.
struct D3 {
  double x, y, z;
};
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 unit3(D3 a) { return a * (1.0 / std::sqrt(dot3(a, a))); }
double clamp1(double x) { return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x); }
double angle_between(D3 a, D3 b) {  // pbrt-v4 AngleBetween
  const double pi = 3.14159265358979323846;
  return dot3(a, b) < 0 ? pi - 2 * std::asin(clamp1(std::sqrt(dot3(a + b, a + b)) / 2))
                        : 2 * std::asin(clamp1(std::sqrt(dot3(b - a, b - a)) / 2));
}
D3 arvo_double(D3 v0, D3 v1, D3 v2, D3 p, double u0, double u1) {
  const double pi = 3.14159265358979323846;
  const D3 a = unit3(v0 - p), b = unit3(v1 - p), c = unit3(v2 - p);
  const D3 nab = unit3(cross3(a, b)), nbc = unit3(cross3(b, c)), nca = unit3(cross3(c, a));
  const double alpha = angle_between(nab, nca * -1), beta = angle_between(nbc, nab * -1), gamma = angle_between(nca, nbc * -1);
  const double ap = (1 - u0) * pi + u0 * (alpha + beta + gamma);
  const double ca = std::cos(alpha), sa = std::sin(alpha);
  const double sp = std::sin(ap) * ca - std::cos(ap) * sa, cph = std::cos(ap) * ca + std::sin(ap) * sa;
  const double k1 = cph + ca, k2 = sp - sa * dot3(a, b);
  const double cbp = clamp1((k2 + (k2 * cph - k1 * sp) * ca) / ((k2 * sp + k1 * cph) * sa));
  const D3 cp = a * cbp + unit3(c - a * dot3(c, a)) * std::sqrt(std::max(0.0, 1 - cbp * cbp));
  const double ct = 1 - u1 * (1 - dot3(cp, b));
  return unit3(b * ct + unit3(cp - b * dot3(cp, b)) * std::sqrt(std::max(0.0, 1 - ct * ct)));
}
bool near(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::fabs(b) + 1e-30; }

ar::SpectrumRef illuminant(float v) { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(v)); }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("nee/point lamp item contribution and target") {
  Rig rig(Emitter::NONE);
  // Two point lamps, pmf 0.25 and 0.75, both above the receiver.
  const ar::Real3 pos[2] = {ar::Real3{ar::Real(0.3), ar::Real(0.2), ar::Real(2)}, ar::Real3{ar::Real(-0.5), ar::Real(0.4), ar::Real(1.5)}};
  rig.use_lamps({ar::light_make(ar::LightType::POINT, pos[0], ar::Real(0), ar::Real(10), illuminant(1.0f)),
                 ar::light_make(ar::LightType::POINT, pos[1], ar::Real(0), ar::Real(6), illuminant(1.0f))},
                {0.25f, 0.75f});
  std::vector<ar::ShadeContext> c(kSlots);
  std::vector<ar::OpenPBRParams> rec(kSlots);
  for (uint32_t s = 0; s < kSlots; ++s) rig.vertex(s, &c[s], &rec[s]);
  const std::vector<ar::Spec> beta = rig.beta;  // the continuation overwrites it
  rig.run();
  uint32_t picked[2] = {0, 0};
  for (uint32_t s = 0; s < kSlots; ++s) {
    const uint32_t it = rig.item_of(s);
    REQUIRE(rig.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW));
    CHECK(rig.sh_target[it] == rig.sample_id[s]);
    // The segment ends at the lamp it picked: which one follows from the direction.
    const ar::Real3 o = rig.sh_o[it], end = o + rig.sh_d[it] * rig.sh_tmax[it];
    const int l = ar::length(end - pos[0]) < ar::length(end - pos[1]) ? 0 : 1;
    ++picked[l];
    CHECK(ar::length(end - pos[l]) < 1e-5f);
    CHECK(o.z > 0.0f);                    // offset off the surface, toward the lamp (0002 D16)
    CHECK(ar::length(o - c[s].P) < 1e-4f);  // the offset: 2^-16 per component near 0 (ray_offset.h)
    // beta f |cos| I / d^2 / (pmf avg(r_u)) from the context point (the hit offset toward wo).
    const ar::Real3 p = ar::offset_ray_origin(c[s].P, c[s].Ng, c[s].wo);
    const ar::Real3 d = pos[l] - p;
    const ar::Real3 wi = ar::normalize(d);
    const ar::LobeEval ev = ar::stack_eval(*rig.kg.e_diel, rec[s], ar::frame_from_normal(c[s].N), c[s].wo, wi,
                                           ar::TransportMode::Radiance);
    const ar::Spec le = ar::light_emission(rig.lamps[uint32_t(l)], c[s].lambda_t, rig.kg.tables->observer) /
                        (ar::Real(4) * ar::kLightPi * ar::dot(d, d));
    const double pm = l == 0 ? 0.25 : 0.75;
    for (int k = 0; k < ar::kLanes; ++k) {
      const double want = double(beta[s].v[k]) * double(ev.f.v[k]) * std::fabs(double(ar::dot(wi, c[s].N))) *
                          double(le.v[k]) / (pm * double(ar::avg(rig.r_u[s])));
      CHECK(want > 0.0);
      CHECK(near(double(rig.sh_contrib[it].v[k]), want, 1e-5));
    }
  }
  CHECK(picked[0] + picked[1] == kSlots);
}

TEST_CASE("nee/rectangle lamp carries the balance weight") {
  Rig rig(Emitter::NONE);
  const ar::LightRecord rect = ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{ar::Real(0.2), 0, ar::Real(1.5)},
                                                   ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, ar::Real(1.0), ar::Real(0.8),
                                                   ar::Real(20), illuminant(1.0f));
  rig.use_lamps({rect}, {1.0f});
  std::vector<ar::ShadeContext> c(kSlots);
  std::vector<ar::OpenPBRParams> rec(kSlots);
  for (uint32_t s = 0; s < kSlots; ++s) rig.vertex(s, &c[s], &rec[s]);
  const std::vector<ar::Spec> beta = rig.beta;  // the continuation overwrites it
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    const uint32_t it = rig.item_of(s);
    REQUIRE(rig.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW));
    CHECK(rig.sh_target[it] == rig.sample_id[s]);
    // The same light sample the stage drew (event block of event index s, 0003 d23).
    const uint32_t e = rig.event_index[s] - 1u;  // the stage advanced it
    const ar::Real2 u{ar::camera_path_draw(rig.kg, rig.sample_id[s], ar::event_dim(e, ar::EV_LIGHT_U)),
                      ar::camera_path_draw(rig.kg, rig.sample_id[s], ar::event_dim(e, ar::EV_LIGHT_V))};
    const ar::LightSampleContext lc = ar::light_context_point(ar::offset_ray_origin(c[s].P, c[s].Ng, c[s].wo), c[s].Ng, c[s].N);
    const ar::LightLiSample ls = ar::light_sample_li(rect, lc, u, c[s].lambda_t, rig.kg.tables->observer);
    REQUIRE(ls.pdf > 0.0f);
    REQUIRE(!ls.delta);
    const ar::LobeEval ev = ar::stack_eval(*rig.kg.e_diel, rec[s], ar::frame_from_normal(c[s].N), c[s].wo, ls.wi,
                                           ar::TransportMode::Radiance);
    REQUIRE(ev.pdf > 0.0f);
    CHECK(ar::length(rig.sh_o[it] + rig.sh_d[it] * rig.sh_tmax[it] - ls.p_light) < 1e-5f);  // no far-end offset on a lamp
    for (int k = 0; k < ar::kLanes; ++k) {
      double den = 0.0;
      for (int j = 0; j < ar::kLanes; ++j) den += double(rig.r_u[s].v[j]) * (double(ls.pdf) + double(ev.pdf));
      den /= ar::kLanes;
      const double want = double(beta[s].v[k]) * double(ev.f.v[k]) * std::fabs(double(ar::dot(ls.wi, c[s].N))) *
                          double(ls.Le.v[k]) / den;
      CHECK(near(double(rig.sh_contrib[it].v[k]), want, 1e-5));
    }
  }
}

TEST_CASE("nee/a lamp below the surface emits no item") {
  Rig rig(Emitter::NONE);
  rig.use_lamps({ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, ar::Real(-2)}, ar::Real(0), ar::Real(10), illuminant(1.0f))},
                {1.0f});
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    const uint32_t it = rig.item_of(s);
    CHECK(rig.sh_next[it] == uint8_t(ar::Stage::SHADOW_TERMINATED));  // the reserved slot stays free
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));  // the path continues
  }
}

TEST_CASE("nee/program emission is evaluated at the light point") {
  // The same emitter as one folded constant (Le in the light record) and as a program (Le 0 with
  // deferred_le): with the same random numbers the items must agree. Skipping the evaluation gives
  // 0; a missing or doubled s gives a factor s (s != 1 here).
  Rig a(Emitter::CONSTANT), b(Emitter::PROGRAM);
  REQUIRE(a.set->lights->radiance_scale != 1.0);
  REQUIRE(a.set->lights->radiance_scale == b.set->lights->radiance_scale);
  REQUIRE(a.set->lights->lights.size() == 2u);
  CHECK((a.set->lights->lights[0].flags & ar::kLightEmissionProgram) == 0u);
  CHECK((b.set->lights->lights[0].flags & ar::kLightEmissionProgram) != 0u);
  a.run();
  b.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    const uint32_t it = a.item_of(s);
    REQUIRE(a.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW));
    REQUIRE(b.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW));
    CHECK(b.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));  // the hit's context came back
    CHECK(ar::length(a.ray_o[s] - b.ray_o[s]) == 0.0f);
    CHECK(ar::length(a.sh_o[it] - b.sh_o[it]) == 0.0f);
    CHECK(a.sh_tmax[it] == b.sh_tmax[it]);
    for (int k = 0; k < ar::kLanes; ++k) {
      CHECK(a.sh_contrib[it].v[k] > 0.0f);
      CHECK(near(double(b.sh_contrib[it].v[k]), double(a.sh_contrib[it].v[k]), 1e-5));
    }
  }
}

TEST_CASE("nee/late delivery lands in slot") {
  // Three unoccluded items, in shadow slots that are not their targets. Their paths have ended and
  // the pool slots were refilled with other samples: delivery reads only the item.
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);
  FilmRequest req;
  req.max_width = 8, req.max_height = 1, req.slot_capacity = 8;
  Film film(dev, FilmLayout::build(req), Owner{1, 1});
  ar::FilmData& f = film.data();
  for (uint32_t i = 0; i < 4u * 8u; ++i) f.slot_L[i] = 0.0f;
  f.slot_L[4u * 6u + 1u] = 0.5f;  // sample 6's path already added emission before it ended
  std::vector<uint8_t> next{uint8_t(ar::Stage::SHADE_SHADOW), uint8_t(ar::Stage::SHADOW_TERMINATED),
                            uint8_t(ar::Stage::SHADE_SHADOW), uint8_t(ar::Stage::SHADE_SHADOW)};
  std::vector<uint32_t> target{6, 99, 1, 4};
  std::vector<ar::Spec> contrib(4);
  for (uint32_t i = 0; i < 4; ++i)
    for (int l = 0; l < ar::kLanes; ++l) contrib[i].v[l] = ar::Real(1 + i) + ar::Real(0.25) * ar::Real(l);
  ar::ShadowItemSoA sh{};
  sh.next_stage = next.data(), sh.target = target.data(), sh.contribution = contrib.data();
  std::vector<uint32_t> sh_queue{3, 0, 2};  // the SHADE_SHADOW segment: shadow slots 3, 0, 2
  ar::StepTable table{};
  ar::KernelGlobals kg{};
  kg.shadow = &sh, kg.shadow_queue = sh_queue.data(), kg.film = &f, kg.step_table = &table;
  cpu_stage_shade_shadow(kg, ar::StepEntry{0, 3, 0}, 0, 3);
  for (uint32_t i : {0u, 2u, 3u}) CHECK(next[i] == uint8_t(ar::Stage::SHADOW_TERMINATED));
  for (int l = 0; l < ar::kLanes; ++l) {
    CHECK(f.slot_L[4u * 6u + uint32_t(l)] == float(contrib[0].v[l]) + (l == 1 ? 0.5f : 0.0f));
    CHECK(f.slot_L[4u * 1u + uint32_t(l)] == float(contrib[2].v[l]));
    CHECK(f.slot_L[4u * 4u + uint32_t(l)] == float(contrib[3].v[l]));
    for (uint32_t z : {0u, 2u, 3u, 5u, 7u}) CHECK(f.slot_L[4u * z + uint32_t(l)] == 0.0f);
  }
}

TEST_CASE("nee/a miss after a diffuse bounce is weighted against world NEE") {
  // Path 0 arrives after a diffuse bounce, path 1 after a delta bounce, path 2 at the camera.
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);
  FilmRequest req;
  req.max_width = 3, req.max_height = 1, req.slot_capacity = 3, req.termination_stats = true;
  Film film(dev, FilmLayout::build(req), Owner{1, 1});
  const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, ar::Real(1));
  ar::LightRecord decoy = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, ar::Real(0), ar::Real(100), c);
  std::vector<ar::LightRecord> lights{decoy, ar::light_make_world(ar::Real(0.8), c)};
  std::vector<float> pmf{0.6f, 0.4f};
  std::vector<ar::AliasBin> bins{{1.0f, 0}, {0.8f, 0}};
  std::vector<uint32_t> bin_light{0, 1};
  ar::LightSamplerView sampler{bins.data(), bin_light.data(), pmf.data(), 2, 2};
  ar::SceneTables tables{};
  std::vector<uint32_t> queue{0, 1, 2}, active{0, 1, 2}, sample_id{0, 1, 2};
  std::vector<uint32_t> flags{0u, ar::LAST_BOUNCE_SPECULAR, 0u};
  std::vector<uint8_t> next(3, uint8_t(ar::Stage::SHADE_BACKGROUND)), term(3, uint8_t(ar::Term::ACTIVE));
  std::vector<ar::U16x6> bounces{ar::U16x6{{2, 2, 0, 0, 0, 0}}, ar::U16x6{{1, 0, 1, 0, 0, 0}}, ar::U16x6{{0, 0, 0, 0, 0, 0}}};
  std::vector<ar::Spec> lambda_t(3, ar::spec_fill(ar::Real(550))), beta(3), r_u(3), r_l(3);
  std::vector<ar::Real3> ray_o(3, ar::Real3{0, 0, 1}), ray_d(3, ar::Real3{0, ar::Real(0.6), ar::Real(0.8)});
  std::vector<uint32_t> prev_ng(3, ar::oct_encode(ar::Real3{0, 0, 1})), prev_ns = prev_ng;
  std::vector<ar::Real> prev_dist(3, ar::Real(0));
  for (uint32_t i = 0; i < 3; ++i)
    for (int l = 0; l < ar::kLanes; ++l) {
      beta[i].v[l] = ar::Real(0.5 + 0.1 * l);
      r_u[i].v[l] = ar::Real(1.0 + 0.5 * l);
      r_l[i].v[l] = ar::Real(0.3 + 0.2 * l);  // r_u / pdf of the bounce
    }
  ar::PathStateSoA path{};
  path.sample_id = sample_id.data(), path.next_stage = next.data(), path.term = term.data(), path.flags = flags.data();
  path.bounces = bounces.data(), path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data();
  path.r_l = r_l.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data(), path.prev_ng = prev_ng.data();
  path.prev_ns = prev_ns.data(), path.prev_dist = prev_dist.data();
  ar::KernelGlobals kg{};
  kg.path = &path, kg.main_queue = queue.data(), kg.active_pixels = active.data(), kg.film = &film.data();
  kg.tables = &tables, kg.lights = lights.data(), kg.light_sampler = &sampler, kg.world_light = 1;
  kg.session.width = 3, kg.session.height = 1, kg.session.samples_per_pass = 1;
  cpu_stage_shade_background(kg, ar::StepEntry{0, 3, 0}, 0, 3);
  const ar::Spec le = ar::light_emission(lights[1], ar::Lambda{{550, 550, 550, 550}}, tables.observer);
  const double p_world = 0.4 / (4.0 * 3.14159265358979323846);
  for (uint32_t i = 0; i < 3; ++i) {
    double den = 0.0;
    for (int l = 0; l < ar::kLanes; ++l) den += double(r_u[i].v[l]) + (i == 0 ? double(r_l[i].v[l]) * p_world : 0.0);
    den /= ar::kLanes;
    for (int l = 0; l < ar::kLanes; ++l)
      CHECK(near(double(film.data().slot_L[4u * i + uint32_t(l)]), double(le.v[l]) * double(beta[i].v[l]) / den, 1e-6));
  }
}


TEST_CASE("nee/spherical triangle sampling matches double") {
  // The float sampler against the double replica of the same map, from 2^17 context points in
  // [-1, 1]^3 below two triangles 3 above (well-shaped, and a sliver), uniforms from the sampler's
  // independent stream. pbrt's float form strayed 1.1e-4 to 1.4e-4 rad at p99.9 (small u.x: the
  // cos(beta') -> sin(beta') step); Peters' steps measure 1.7e-6 to 2.6e-6.
  const ar::Real3 tris[3][3] = {{{-0.6f, -0.6f, 3.0f}, {0.6f, -0.5f, 3.1f}, {0.0f, 0.6f, 3.05f}},
                                {{0.2f, 0.1f, 3.3f}, {1.0f, 0.2f, 3.2f}, {0.5f, 0.9f, 3.25f}},
                                {{-0.9f, 0.0f, 3.0f}, {0.9f, 0.05f, 3.2f}, {0.0f, 0.12f, 3.1f}}};
  for (int t = 0; t < 3; ++t) {
    const ar::LightRecord r = ar::light_make_triangle(tris[t][0], tris[t][1], tris[t][2], 0, 0, ar::kLightTwoSided, 1.0f,
                                                      illuminant(1.0f));
    const D3 v[3] = {{tris[t][0].x, tris[t][0].y, tris[t][0].z}, {tris[t][1].x, tris[t][1].y, tris[t][1].z},
                     {tris[t][2].x, tris[t][2].y, tris[t][2].z}};
    std::vector<double> ang;
    for (uint32_t i = 0; i < (1u << 17); ++i) {
      const auto unit = [&](uint32_t d) { return ar::sample_independent(i, uint32_t(t), 0, d, 74u); };
      const ar::Real3 p{2 * unit(0) - 1, 2 * unit(1) - 1, 2 * unit(2) - 1};
      const ar::Real2 u{unit(3), unit(4)};
      ar::Real3 q{0, 0, 0};
      REQUIRE(ar::tri_sample_spherical(r, p, u, &q));
      const ar::Real3 wf = ar::normalize(q - p);
      const D3 wd = arvo_double(v[0], v[1], v[2], D3{p.x, p.y, p.z}, u.x, u.y), w{wf.x, wf.y, wf.z};
      const D3 cr = cross3(w, wd);
      ang.push_back(std::atan2(std::sqrt(dot3(cr, cr)), dot3(w, wd)));
    }
    std::sort(ang.begin(), ang.end());
    const double p999 = ang[size_t(0.999 * double(ang.size()))], worst = ang.back();
    MESSAGE("triangle " << t << ": p99.9 " << p999 << " rad, max " << worst << " rad");
    CHECK(p999 < 5e-6);  // half the agreement test's direction limit (0002 D12 class DIRECTION, 1e-5)
  }
}
}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("nee/alias test frequency at 2^20 bins") {
  // 2^20 bins that keep light A with probability q = 1e-4 and otherwise give bin 1, whose light is B,
  // so A's pmf is q (2^20 - 1) / 2^20. The alias test's uniform is independent of the 32-bit bin (RULINGS.md 73);
  // the remainder of bits * n would leave it 12 bits at this bin count and pick A with probability
  // 1 / 4096 = 2.4 q. 16 passes of 2^16 paths through the stage, a different event index each.
  constexpr uint32_t kPaths = 1u << 16, kPasses = 16, kBins = 1u << 20;
  constexpr float kQ = 1e-4f;
  Rig rig(Emitter::NONE, kPaths);
  const ar::Real3 a{ar::Real(0.3), 0, ar::Real(2)}, b{ar::Real(-0.4), ar::Real(0.2), ar::Real(2)};
  rig.use_lamps({ar::light_make(ar::LightType::POINT, a, 0, ar::Real(10), illuminant(1.0f)),
                 ar::light_make(ar::LightType::POINT, b, 0, ar::Real(10), illuminant(1.0f))},
                {kQ, 1.0f - kQ});
  rig.bins.assign(kBins, ar::AliasBin{kQ, 1u});  // the alias is bin 1, which always gives light B
  rig.bin_light.assign(kBins, 0u);
  rig.bins[1] = ar::AliasBin{1.0f, 1u}, rig.bin_light[1] = 1u;
  rig.pmf[0] = float(double(kQ) * double(kBins - 1) / double(kBins)), rig.pmf[1] = 1.0f - rig.pmf[0];
  rig.sampler = ar::LightSamplerView{rig.bins.data(), rig.bin_light.data(), rig.pmf.data(), kBins, 2};
  const std::vector<ar::Real3> ray_d = rig.ray_d;
  uint64_t picked_a = 0, items = 0;
  for (uint32_t pass = 0; pass < kPasses; ++pass) {
    for (uint32_t s = 0; s < kPaths; ++s) {
      rig.event_index[s] = uint16_t(pass), rig.bounces[s] = ar::U16x6{{0, 0, 0, 0, 0, 0}};
      rig.next_stage[s] = uint8_t(ar::Stage::SHADE_SURFACE), rig.ray_d[s] = ray_d[s];
      rig.sh_next[s] = uint8_t(ar::Stage::SHADOW_TERMINATED);
    }
    rig.run();
    for (uint32_t s = 0; s < kPaths; ++s) {
      if (rig.sh_next[s] != uint8_t(ar::Stage::INTERSECT_SHADOW)) continue;
      ++items;
      const ar::Real3 end = rig.sh_o[s] + rig.sh_d[s] * rig.sh_tmax[s];
      picked_a += ar::length(end - a) < ar::length(end - b);
    }
  }
  REQUIRE(items == uint64_t(kPaths) * kPasses);  // both lamps light every path
  const double mean = double(rig.pmf[0]) * double(items), sd = std::sqrt(mean);
  MESSAGE("light A picked " << picked_a << " times, expected " << mean << " +- " << sd);
  CHECK(std::fabs(double(picked_a) - mean) < 5.0 * sd);
}

}  // TEST_SUITE