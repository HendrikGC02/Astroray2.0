// SPDX-License-Identifier: Apache-2.0
// shade_surface_ext (ShadeKind::Ext) on hand-built hits of a real scene: a horizontal quad whose material
// is a reflective GRATING compiled and uploaded by the store. The scene holds two distinct gratings and
// the quad uses the second, so its table sits at a non-zero TableArena offset. Order 0 keeps all four
// lanes along the mirror direction; any other order follows the grating equation at lambda_t[0] and
// terminates the secondaries (0003 d5); the mean of beta per order over 2^16 draws is the table's
// efficiency at lambda_t[0] (0005 item 22: f |cos| / pdf = eta / p_m, chosen with p_m); every order
// evanescent ends ZERO_THROUGHPUT; the stage reaches neither the lobe stack nor a light (poisoned E
// tables, a lamp of NaN strength), and the guide tint stays as it was.
// Ref: docs/decisions/0005-material-model.md items 15-17, 21-23; 0003 d5, d23; Palmer, Diffraction
// Grating Handbook, eq. 2-3 (the conical grating equation, item 21's direction-cosine form).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "host/film/film_layout.h"
#include "host/material/grating_authored.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_surface_ext.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr ar_key kMesh = 100, kGratingA = 1, kGratingB = 2;  // B, the quad's, is placed second
constexpr uint32_t kMaxBounces = 8;
constexpr double kLane[4] = {430.0, 500.0, 570.0, 640.0};

// A CD-like sinusoidal grating, orders -2..2 (0005 items 23-24).
ar_material_desc grating(float depth_nm) {
  GratingAuthored g = {};
  g.pitch_nm = 1600.0f, g.depth_nm = depth_nm, g.coating_n = 1.0f, g.coating_k = 6.0f, g.m_min = -2, g.m_max = 2;
  ar_material_desc d = {};
  d.ext_kind = uint32_t(ar::ExtKind::GRATING);
  std::memcpy(d.ext_constants, &g, sizeof g);
  return d;
}

// One quad of half-size 1 at z = 0 facing +z under grating B, every path hitting it at the same point
// from the same direction with the same lanes.
struct Rig {
  ContextState sctx;
  Scene scene{sctx, SceneConfig{2, {}, {}}};
  std::shared_ptr<const SceneTableSet> set;
  std::shared_ptr<const MaterialArrays> mats;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::SceneData sd{};
  std::vector<ar::ExtParams> ext;
  ar::PathStateSoA path{};
  ar::LightSamplerView no_lights{};
  ar::ShadowItemSoA shadow{};
  std::vector<uint32_t> sh_queue, sh_target;
  std::vector<uint8_t> sh_next;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<ar::Real> sh_tmax;
  std::vector<ar::Spec> sh_contrib;
  std::vector<ar::LightRecord> lamps;
  std::vector<ar::AliasBin> bins;
  std::vector<uint32_t> bin_light;
  std::vector<float> pmf;
  ar::LightSamplerView lamp_sampler{};
  std::vector<float> nan_table;
  ar::EDielTable nan_diel{};
  ar::ESpecTable nan_spec{};
  uint32_t n;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d, tint;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;
  const ar::Real3 d_in = ar::normalize(ar::Real3{ar::Real(0.15), ar::Real(-0.3), ar::Real(-1)});  // every path's ray

  explicit Rig(uint32_t count, bool guides = false) : n(count) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      const ar_material_desc a = grating(90.0f), b = grating(120.0f);
      REQUIRE(scene.materials->set_material(e, kGratingA, &a) == AR_OK);
      REQUIRE(scene.materials->set_material(e, kGratingB, &b) == AR_OK);
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
      const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
      ar_mesh_desc md = {};
      md.key = kMesh, md.vertex_count = 4, md.corner_count = 4, md.face_count = 1, md.motion_steps = 1;
      ar_mesh_buffers mb = {};
      REQUIRE(mesh_begin(scene, e, &md, &mb) == AR_OK);
      std::memcpy(*mb.positions, pos.data(), pos.size() * 4);
      std::memcpy(*mb.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*mb.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < mb.corner_normals_count; ++i) (*mb.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(kMesh) == AR_OK);
    });
    commit([&](Edit& e) {
      std::vector<ar_key> geom{kMesh}, object{500}, keys{kGratingB};
      std::vector<ar_xform> xf(1);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{1};
      std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(1, 0);
      ar_instances_desc d = {};
      d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 1;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);
    mats = scene.materials->arrays();

    FilmRequest req;
    req.max_width = n;
    req.max_height = 1;
    req.slot_capacity = n;
    req.statistics = true;
    req.termination_stats = true;
    req.guides = guides;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(0u, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE_EXT)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u);
      event_index.push_back(0);
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{ar::Real(0.25), ar::Real(0.25)});
      hit_t.push_back(ar::Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1});
      ray_d.push_back(d_in);
      eta_scale.push_back(ar::Real(1));
      prev_ng.push_back(0u), prev_ns.push_back(0u), prev_dist.push_back(ar::Real(7));
      tint.push_back(ar::Real3{ar::Real(0.3), ar::Real(0.5), ar::Real(0.7)});
      ar::Spec lt, one;
      for (int l = 0; l < ar::kLanes; ++l) lt.v[l] = ar::Real(kLane[l]), one.v[l] = ar::Real(1);
      lambda_t.push_back(lt), beta.push_back(one), r_u.push_back(one), r_l.push_back(one);
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();
    if (guides) path.guide_tint = tint.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 23;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = kMaxBounces;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.light_sampler = &no_lights;
    // The ExtParams in host memory, so a case can change a grating's words.
    sd = *kg.scene;
    ext.assign(sd.ext_params, sd.ext_params + sd.ext_param_count);
    sd.ext_params = ext.data();
    kg.scene = &sd;
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
  // The quad's grating: header index of key B.
  uint32_t material_b() const { return 1u; }
  ar::GratingParams params_b() const { return ar::grating_params(ext[ar::ext_index(mats->headers[material_b()])]); }
  void set_params_b(const ar::GratingParams& g) { ext[ar::ext_index(mats->headers[material_b()])] = ar::grating_ext_params(g); }
  // Grating B's table, read from the arena on the host side at its own offset.
  ar::GratingTable table_b() const {
    const ar::GratingParams g = params_b();
    return ar::GratingTable{reinterpret_cast<const float*>(mats->table_arena.data() + g.table), g.m_min, g.m_max - g.m_min + 1};
  }
  void run() { cpu_stage_shade_surface_ext(kg, ar::StepEntry{0, n, 0}, 0, n); }
  void run_common() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  // One point lamp above the quad, the only light (pmf 1), of NaN strength: a light sample of it gives a
  // non-finite contribution, which ends the path INVALID.
  void use_nan_lamp() {
    for (uint32_t i = 0; i < n; ++i) {
      sh_queue.push_back(i), sh_target.push_back(~0u);
      sh_next.push_back(uint8_t(ar::Stage::SHADOW_TERMINATED));
      sh_o.push_back(ar::Real3{0, 0, 0}), sh_d.push_back(ar::Real3{0, 0, 0}), sh_tmax.push_back(ar::Real(-1));
      sh_contrib.push_back(ar::spec_fill(ar::Real(0)));
    }
    shadow.next_stage = sh_next.data(), shadow.ray_o = sh_o.data(), shadow.ray_d = sh_d.data();
    shadow.tmax = sh_tmax.data(), shadow.contribution = sh_contrib.data(), shadow.target = sh_target.data();
    lamps.push_back(ar::light_make(ar::LightType::POINT, ar::Real3{ar::Real(0.3), ar::Real(0.2), ar::Real(2)}, ar::Real(0),
                                   ar::Real(std::numeric_limits<float>::quiet_NaN()),
                                   ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1))));
    pmf = {1.0f};
    bins.push_back({1.0f, 0u}), bin_light.push_back(0u);
    lamp_sampler = ar::LightSamplerView{bins.data(), bin_light.data(), pmf.data(), 1u, 1u};
    kg.shadow = &shadow, kg.shadow_queue = sh_queue.data();
    kg.lights = lamps.data(), kg.light_sampler = &lamp_sampler;
  }
  // E tables of NaN: any lobe-stack call gives a non-finite value, which ends the path INVALID.
  void use_nan_e_tables() {
    nan_table.assign(size_t(ar::kEDielN) * ar::kEDielN * ar::kEDielN + size_t(ar::kESpecN) * ar::kESpecN,
                     std::numeric_limits<float>::quiet_NaN());
    nan_diel.e = nan_table.data(), nan_spec.e = nan_table.data();
    kg.e_diel = &nan_diel, kg.e_spec = &nan_spec;
  }
  uint32_t hist(ar::Term t) const {
    const ar::FilmData& f = film->data();
    uint32_t total = 0;
    for (uint32_t p = 0; p < n; ++p) total += f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + p];
    return total;
  }
  ar::Real3 wo() const { return -d_in; }
};

ar::Real3 mirror(ar::Real3 wo) { return ar::Real3{-wo.x, -wo.y, wo.z}; }  // about N = +z

double dist(ar::Real3 a, ar::Real3 b) {
  const double x = double(a.x) - double(b.x), y = double(a.y) - double(b.y), z = double(a.z) - double(b.z);
  return std::sqrt(x * x + y * y + z * z);
}

// Item 21 for a reflection off the quad's grating, computed here in double: the frame of N = +z and the
// default tangent (core/frame.h for +z gives x), grooves along x and the grating vector along y, so
// t_o = -t_i + m (lambda / d) y. False when the order is evanescent.
bool order_direction(ar::Real3 wo, int m, double lambda_nm, double pitch_nm, ar::Real3* wi) {
  const double tx = -double(wo.x), ty = -double(wo.y) + double(m) * lambda_nm / pitch_nm;
  const double t2 = tx * tx + ty * ty;
  if (t2 > 1.0) return false;
  *wi = ar::Real3{ar::Real(tx), ar::Real(ty), ar::Real(std::sqrt(1.0 - t2))};
  return true;
}

// The order whose direction at lambda_nm is within tol of wi, or 99.
int order_of(ar::Real3 wo, ar::Real3 wi, double lambda_nm, double pitch_nm, int m_min, int m_max, double tol) {
  for (int m = m_min; m <= m_max; ++m) {
    ar::Real3 d{};
    if (order_direction(wo, m, lambda_nm, pitch_nm, &d) && dist(d, wi) <= tol) return m;
  }
  return 99;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_surface_ext/order zero keeps four lanes") {
  Rig rig(1024);
  const ar::GratingParams g = rig.params_b();
  REQUIRE(g.table != 0u);  // grating B sits after grating A in the arena
  rig.run();
  const ar::Real3 wo = rig.wo(), want = mirror(wo);
  uint32_t zero = 0;
  for (uint32_t s = 0; s < rig.n; ++s) {
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE) || dist(rig.ray_d[s], want) > 1e-5) continue;
    ++zero;
    INFO("slot " << s);
    CHECK((rig.flags[s] & ar::SECONDARIES_TERMINATED) == 0u);
    CHECK((rig.flags[s] & ar::LAST_BOUNCE_SPECULAR) != 0u);
    CHECK(rig.ray_type[s] == ar::kRayTypeGlossy);
    CHECK(rig.bounces[s].v[2] == 1u);
    for (int l = 0; l < ar::kLanes; ++l) {
      CHECK(double(rig.lambda_t[s].v[l]) == kLane[l]);
      CHECK(rig.beta[s].v[l] > 0.0f);  // every lane goes on
    }
  }
  CHECK(zero > 100u);  // order 0 carries most of the energy at this pitch
  CHECK(rig.hist(ar::Term::INVALID) == 0u);
}

TEST_CASE("shade_surface_ext/order one terminates secondaries") {
  Rig rig(1024);
  const ar::GratingParams g = rig.params_b();
  rig.run();
  const ar::Real3 wo = rig.wo();
  uint32_t others = 0;
  for (uint32_t s = 0; s < rig.n; ++s) {
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE) || dist(rig.ray_d[s], mirror(wo)) <= 1e-5) continue;
    ++others;
    INFO("slot " << s);
    // The direction is the grating equation's for a non-zero order at lambda_t[0], to 1e-5.
    const int m = order_of(wo, rig.ray_d[s], kLane[0], double(g.pitch_nm), g.m_min, g.m_max, 1e-5);
    CHECK(m != 99);
    CHECK(m != 0);
    CHECK((rig.flags[s] & ar::SECONDARIES_TERMINATED) != 0u);
    for (int l = 0; l < ar::kLanes; ++l) {
      CHECK(double(rig.lambda_t[s].v[l]) == kLane[0]);   // every lane evaluates at lambda_t[0]
      CHECK(rig.beta[s].v[l] == rig.beta[s].v[0]);
      CHECK(rig.r_u[s].v[l] == ar::Real(1));               // r_u and r_l keep their ratios (0003 d7)
    }
    CHECK(rig.beta[s].v[0] > 0.0f);
  }
  CHECK(others > 20u);
  CHECK(rig.hist(ar::Term::INVALID) == 0u);
}

TEST_CASE("shade_surface_ext/beta mean equals efficiency") {
  // E[beta_0 1{order m}] = p_m eta_m(lambda_0) / p_m = eta_m(lambda_0), the table at |cos theta_o|
  // (bilinear, grating_table.h), 0 where order m is evanescent at lambda_0. Russian roulette starts
  // at depth 2, so the first bounce keeps every path.
  constexpr uint32_t kN = 1u << 16;
  Rig rig(kN);
  const ar::GratingParams g = rig.params_b();
  const ar::GratingTable tab = rig.table_b();
  rig.run();
  const ar::Real3 wo = rig.wo();
  const int orders = g.m_max - g.m_min + 1;
  std::vector<double> sum(size_t(orders), 0.0), sum2(size_t(orders), 0.0);
  uint32_t unmatched = 0;
  for (uint32_t s = 0; s < kN; ++s) {
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE)) continue;
    const int m = order_of(wo, rig.ray_d[s], kLane[0], double(g.pitch_nm), g.m_min, g.m_max, 1e-5);
    if (m == 99) {
      ++unmatched;
      continue;
    }
    const double b = double(rig.beta[s].v[0]);
    sum[size_t(m - g.m_min)] += b, sum2[size_t(m - g.m_min)] += b * b;
  }
  CHECK(unmatched == 0u);
  CHECK(rig.hist(ar::Term::INVALID) == 0u);
  const double cos_o = std::fabs(double(wo.z));
  uint32_t checked = 0;
  for (int m = g.m_min; m <= g.m_max; ++m) {
    ar::Real3 d{};
    const double want = order_direction(wo, m, kLane[0], double(g.pitch_nm), &d)
                            ? double(ar::grating_table_eta(tab, m, ar::Real(kLane[0]), ar::Real(cos_o)))
                            : 0.0;
    const double mean = sum[size_t(m - g.m_min)] / double(kN);
    const double var = sum2[size_t(m - g.m_min)] / double(kN) - mean * mean;
    const double se = std::sqrt(std::max(var, 0.0) / double(kN));
    INFO("order " << m << " mean " << mean << " eta " << want << " se " << se);
    if (want == 0.0) {
      CHECK(mean == 0.0);
      continue;
    }
    ++checked;
    REQUIRE(se > 0.0);
    CHECK(std::fabs(mean - want) <= 3.0 * se);
  }
  CHECK(checked >= 3u);
}

TEST_CASE("shade_surface_ext/every order evanescent ends zero throughput") {
  // Orders 1..2 of a 150 nm pitch: m lambda / d >= 2.4 for every lane, so |t_o| > 1 for both.
  Rig rig(64);
  ar::GratingParams g = rig.params_b();
  g.pitch_nm = 150.0f, g.m_min = 1, g.m_max = 2;
  rig.set_params_b(g);
  rig.run();
  CHECK(rig.hist(ar::Term::ZERO_THROUGHPUT) == rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) CHECK(rig.term[s] == uint8_t(ar::Term::ZERO_THROUGHPUT));
}

TEST_CASE("shade_surface_ext/never reaches the lobe stack") {
  // NaN E tables: the Common identity on the same hits fails every sample (the poison bites); the Ext
  // identity never calls the stack, so no sample is invalid.
  {
    Rig common(64);
    common.use_nan_e_tables();
    common.run_common();
    CHECK(common.hist(ar::Term::INVALID) == common.n);
  }
  Rig rig(64);
  rig.use_nan_e_tables();
  rig.run();
  CHECK(rig.hist(ar::Term::INVALID) == 0u);
  uint32_t active = 0;
  for (uint32_t s = 0; s < rig.n; ++s) active += rig.term[s] == uint8_t(ar::Term::ACTIVE) ? 1u : 0u;
  CHECK(active == rig.n);
}

TEST_CASE("shade_surface_ext/a grating hit selects no light") {
  // A lamp of NaN strength: a light sample of it fails the sample, as the Common identity shows on the
  // same hits; a grating hit picks no light at all (its kind is all delta), so none is invalid and no
  // shadow item is written.
  {
    Rig common(64);
    common.use_nan_lamp();
    common.run_common();
    CHECK(common.hist(ar::Term::INVALID) == common.n);
  }
  Rig rig(64);
  rig.use_nan_lamp();
  rig.run();
  CHECK(rig.hist(ar::Term::INVALID) == 0u);
  for (uint32_t s = 0; s < rig.n; ++s) {
    CHECK(rig.term[s] == uint8_t(ar::Term::ACTIVE));
    CHECK(rig.sh_next[s] == uint8_t(ar::Stage::SHADOW_TERMINATED));
  }
}

TEST_CASE("shade_surface_ext/a grating hit leaves the guide tint unchanged") {
  // PROVISIONAL guide rule (0005 item 17, 0008 d15): a grating sample is delta and leaves the tint as
  // it was; nothing is written to the guide planes and the hit is not the denoising surface.
  Rig rig(64, true);
  REQUIRE(rig.kg.film->guide_albedo >= 0);
  rig.run();
  const ar::FilmData& f = rig.film->data();
  for (uint32_t s = 0; s < rig.n; ++s) {
    INFO("slot " << s);
    REQUIRE(rig.term[s] == uint8_t(ar::Term::ACTIVE));
    CHECK(rig.tint[s].x == ar::Real(0.3));
    CHECK(rig.tint[s].y == ar::Real(0.5));
    CHECK(rig.tint[s].z == ar::Real(0.7));
    CHECK((rig.flags[s] & ar::DENOISE_SURFACE_DONE) == 0u);
    for (uint32_t k = 0; k < 6; ++k) CHECK(f.add[uint64_t(uint32_t(f.guide_albedo) + k) * f.plane_stride + s] == 0.0f);
  }
}

TEST_CASE("shade_surface_ext/the cpu session device runs the stage") {
  ContextState ctx;
  std::shared_ptr<Device> dev = make_cpu_session_device(ctx, 1);
  CHECK(static_cast<const CpuDevice&>(*dev).stage_fn(ar::Stage::SHADE_SURFACE_EXT) == &cpu_stage_shade_surface_ext);
}

}  // TEST_SUITE
