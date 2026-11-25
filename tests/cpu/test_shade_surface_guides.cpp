// SPDX-License-Identifier: Apache-2.0
// Denoiser guides (0008 d15, d22, d23; V10): hand-built hits on a real scene's tables, the rig of
// test_shade_surface_bsdf.cpp with two quads: a "front" quad at z = 0 facing +z and a wall at
// z = 2 facing -z. A fronto-parallel constant plane at 1 spp gives bitwise identical albedo and
// normal guides for two seeds and two sets of wavelengths (no wavelength noise); a delta metal
// mirror (base_color (1, 0.78, 0.34)) in front of a diffuse wall tints the wall's albedo by the
// mirror's RGB albedo and sets DENOISE_SURFACE_DONE at the wall, not at the mirror; a smooth
// dielectric with no diffuse base (the "glass": the stack has no transmission lobe yet) tints by
// specular_color E_diel; a path whose only hit is delta and that escapes writes Cycles' background
// value (tint times its contribution in display RGB, no normal); a film without guides writes
// nothing; a material whose base_color is set per point (a UV-driven colour) writes magenta.
// The film is read through FilmView after one resolve (each pixel holds one sample).
// Ref: docs/decisions/0008-film-outputs-denoise.md d15, d22, d23, V10; 0003 d21; Cycles
// kernel/film/denoising_passes.h and light_passes.h (behaviour, not pixels).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "../gpu/agree.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "film/resolve.h"
#include "host/film/film_layout.h"
#include "host/film/film_view.h"
#include "host/film/response_table.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_background.h"
#include "kernel/stages/shade_surface.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Real;
using ar::Real3;

namespace {

constexpr ar_key kMesh = 100, kFront = 1, kWall = 2;

struct Rig {
  ContextState sctx;
  Scene scene{sctx, SceneConfig{2, {}, {}}};
  std::shared_ptr<const SceneTableSet> set;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ResponseTable response;
  WavelengthTable wavelengths;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::SceneData sd{};
  std::vector<ar::ConstantBlock> blocks;
  ar::PathStateSoA path{};
  ar::LightSamplerView no_lights{};
  uint32_t n;
  bool guides_on;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<Real3> ray_o, ray_d, tint;
  std::vector<ar::Real2> hit_uv;
  std::vector<Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;

  // `front` is the front quad's surface graph (null: an Emission graph whose block the test replaces).
  Rig(uint32_t count, bool guides, const ar_graph_desc* front = nullptr) : n(count), guides_on(guides) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      std::vector<ar_graph_node> nodes(3);
      std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}};
      std::vector<float> f{1.0f, 1.0f, 1.0f, 1.0f};
      nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_first = 0, nodes[0].f32_count = 3;
      nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
      nodes[2].op = uint32_t(ar::Op::EMIT_LE);
      ar_graph_desc d = {};
      d.node_count = 3, d.link_count = 2, d.f32_count = 4, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
      for (ar_key k : {kFront, kWall}) {
        REQUIRE(scene.materials->set_graph(e, k + 1000, k == kFront && front != nullptr ? front : &d) == AR_OK);
        ar_material_desc m = {};
        m.surface_graph = k + 1000;
        m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
        REQUIRE(scene.materials->set_material(e, k, &m) == AR_OK);
      }
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
      const std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 1};
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
      // Instance 0: the front quad as authored. Instance 1: the wall, (x, y, z) -> (x, -y, 2 - z), a
      // rotation about x, so it faces -z.
      std::vector<ar_key> geom{kMesh, kMesh}, object{500, 501}, keys{kFront, kWall};
      std::vector<ar_xform> xf(2);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      xf[1].m[0] = 1.0f, xf[1].m[5] = -1.0f, xf[1].m[10] = -1.0f, xf[1].m[11] = 2.0f;
      std::vector<uint32_t> zeros(2, 0), vis(2, AR_VISIBILITY_CAMERA), moff{0, 1}, mcount{1, 1};
      std::vector<ar_rgba> colors(2, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(2, 0);
      ar_instances_desc d = {};
      d.instance_count = 2, d.motion_steps = 1, d.transform_count = 2, d.material_key_count = 2;
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
    req.guides = guides;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 0.25;  // s = 4, so a missing 1 / s shows
    // Flat display tables (test_shade_background.cpp's): the resolve only needs valid views here,
    // and the escape case converts through the same ones.
    response.display_first = 0;
    response.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    response.y_w = 1.0;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) response.m_ws[i] = m[i];
    for (uint32_t c = 0; c < 3; ++c) {
      response.channels.push_back({2 * c, 2, ar::RESPONSE_ENERGY, 0});
      response.knot_nm.push_back(300.0f);
      response.knot_nm.push_back(900.0f);
      response.knot_value.push_back(1.0f + 0.5f * float(c));
      response.knot_value.push_back(1.0f + 0.25f * float(c));
    }
    wavelengths.edges = {360.0f, 616.0f, 830.0f};
    wavelengths.cdf = {0.0f, 0.4f, 1.0f};
    film->data().response = response_view(response);
    film->data().wavelengths = {wavelengths.edges.data(), wavelengths.cdf.data(), wavelengths.bins()};

    queue.resize(n), active.resize(n), sample_id.resize(n), flags.resize(n), hit_prim.resize(n), hit_inst.resize(n);
    prev_ng.resize(n), prev_ns.resize(n), next_stage.resize(n), term.resize(n), ray_type.resize(n);
    event_index.resize(n), bounces.resize(n), ray_o.resize(n), ray_d.resize(n), tint.resize(n), hit_uv.resize(n);
    hit_t.resize(n), eta_scale.resize(n), prev_dist.resize(n), lambda_t.resize(n), beta.resize(n), r_u.resize(n), r_l.resize(n);
    for (uint32_t i = 0; i < n; ++i) queue[i] = active[i] = sample_id[i] = i;
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();
    // The tint array exists in both modes, so a stage that touched it with guides off would show.
    path.guide_tint = tint.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    in.uniforms.features.guides = guides;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.light_sampler = &no_lights;  // no NEE: these cases test the guides
    sd = *kg.scene;
    blocks.assign(sd.material_constants, sd.material_constants + kg.material_constant_count);
    sd.material_constants = blocks.data();
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
  ar::ConstantBlock& block(uint32_t inst) { return blocks[sd.material_headers[ar::hit_material_id(kg, inst, 0)].const_offset]; }
  const ar::Rgb2SpecTable& table() const { return scene.materials->table(); }
  ar::SpectrumRef rgb(float r, float g, float b) const {
    ar::SpectrumRef ref{};
    uint32_t fl = 0;
    REQUIRE(ar::make_rgb_ref(table(), ar::Rgb{{Real(r), Real(g), Real(b)}}, ar::SpectrumSemantic::REFLECTANCE, &ref, &fl));
    return ref;
  }
  // Colour slots with their exact guide RGB, as the compiler stores them.
  void base_color(uint32_t inst, float r, float g, float b) {
    block(inst).base_color = rgb(r, g, b);
    const float c[3] = {r, g, b};
    std::memcpy(block(inst).guide_base_color, c, sizeof c);
  }
  void specular_color(uint32_t inst, float r, float g, float b) {
    block(inst).specular_color = rgb(r, g, b);
    const float c[3] = {r, g, b};
    std::memcpy(block(inst).guide_specular_color, c, sizeof c);
  }
  // A diffuse wall with no specular layer.
  void diffuse_wall() {
    ar::ConstantBlock& b = block(1);
    b.base_weight = 1.0f, b.base_diffuse_roughness = 0.0f, b.base_metalness = 0.0f, b.specular_weight = 0.0f;
    b.emission_luminance = 0.0f;
    base_color(1, 0.2f, 0.6f, 0.9f);
  }
  // The path of slot i at its first vertex on instance `inst` at world point p (on the quad), with
  // direction d and lanes from `rng`.
  void start(uint32_t i, uint32_t inst, Real3 p, Real3 d, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    flags[i] = ar::set_hero_lane(0u, i & 3u);
    next_stage[i] = uint8_t(ar::Stage::SHADE_SURFACE), term[i] = uint8_t(ar::Term::ACTIVE);
    ray_type[i] = 1u, event_index[i] = 0, bounces[i] = ar::U16x6{{0, 0, 0, 0, 0, 0}};
    ray_o[i] = p - d, ray_d[i] = d;
    eta_scale[i] = Real(1), prev_ng[i] = prev_ns[i] = 0u, prev_dist[i] = Real(0), hit_t[i] = Real(1);
    tint[i] = guides_on ? Real3{Real(1), Real(1), Real(1)} : Real3{Real(-7), Real(-7), Real(-7)};  // generate_camera's
    for (int l = 0; l < ar::kLanes; ++l) lambda_t[i].v[l] = Real(380.0 + 400.0 * uni(rng));
    beta[i] = r_u[i] = r_l[i] = ar::spec_fill(Real(1));
    hit(i, inst, p);
  }
  // Hit fields of slot i for world point p on instance inst; the quad's two triangles are
  // (0, 1, 2) and (0, 2, 3) and hit_uv weighs corners 1 and 2. The context the stage builds from
  // them must land on p.
  void hit(uint32_t i, uint32_t inst, Real3 p) {
    const Real lx = p.x, ly = inst == 0 ? p.y : -p.y;
    hit_inst[i] = inst;
    if (lx >= ly) {
      const Real v = (ly + Real(1)) / Real(2);
      hit_prim[i] = 0u, hit_uv[i] = ar::Real2{(lx + Real(1)) / Real(2) - v, v};
    } else {
      const Real u = (lx + Real(1)) / Real(2);
      hit_prim[i] = 1u, hit_uv[i] = ar::Real2{u, (ly + Real(1)) / Real(2) - u};
    }
    ar::ShadeContext c{};
    REQUIRE(ar::shade_context_build(kg, i, &c));
    REQUIRE(std::fabs(double(c.P.x - p.x)) < 1e-5);
    REQUIRE(std::fabs(double(c.P.y - p.y)) < 1e-5);
    REQUIRE(std::fabs(double(c.P.z - p.z)) < 1e-5);
  }
  // Every live path continues to the wall at z = 2 (its next ray from the stage).
  void to_wall() {
    for (uint32_t i = 0; i < n; ++i) {
      REQUIRE(next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
      const Real t = (Real(2) - ray_o[i].z) / ray_d[i].z;
      hit(i, 1, ray_o[i] + ray_d[i] * t);
      next_stage[i] = uint8_t(ar::Stage::SHADE_SURFACE);
    }
  }
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  void resolve() {
    film->check_resolve_fits(1);
    cpu_film_resolve(kg, 0, ar::film_resolve_count(kg));
  }
};

// The diffuse wall's albedo, exactly: no specular layer (E_spec = 0) and EON at roughness 0 returns
// rho (eon.h: E_FON = 1), so the guide is the base colour.
constexpr double kWallAlbedo[3] = {0.2, 0.6, 0.9};

struct Guides {
  std::vector<double> albedo[3], normal[3];
};
Guides read_guides(Rig& rig) {
  rig.resolve();
  const FilmView v(rig.dev, *rig.film);
  Guides g;
  for (uint32_t c = 0; c < 3; ++c) g.albedo[c] = v.mean("guide_albedo", c), g.normal[c] = v.mean("guide_normal", c);
  return g;
}

double comp(Real3 v, uint32_t c) { return double(c == 0 ? v.x : c == 1 ? v.y : v.z); }

// Front quad points with x > |y|: both quads' triangle 0 holds them and their mirror images.
Real3 front_point(uint32_t i, uint32_t n) {
  const double t = (double(i) + 0.5) / double(n);
  return Real3{Real(0.15 + 0.3 * t), Real(-0.1 + 0.2 * std::fmod(7.0 * t, 1.0)), Real(0)};
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_surface_guides/constant plane bitwise") {
  // V10: a fronto-parallel plane (a GGX layer over EON, roughness 0.3, so every sample is non-delta)
  // at 1 spp: two seeds and two sets of wavelengths give the same guides to the bit.
  constexpr uint32_t kN = 64;
  Guides g[2];
  for (int run = 0; run < 2; ++run) {
    Rig rig(kN, true);
    rig.kg.session.seed = run == 0 ? 11u : 23u;
    ar::ConstantBlock& b = rig.block(0);
    b.base_weight = 1.0f, b.specular_weight = 1.0f, b.specular_roughness = 0.3f, b.emission_luminance = 0.0f;
    rig.base_color(0, 0.6f, 0.3f, 0.1f);
    rig.specular_color(0, 0.9f, 0.95f, 1.0f);
    std::mt19937_64 rng(uint64_t(run) + 1u);
    for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
    // From the albedo tables directly (0008 d15): diffuse = E_eon (1 - E_spec) and specular = E_spec,
    // E_spec = specular_color E_diel(1, r, 1.5) at alpha = 0.3^2, E_eon at roughness 0.
    const Real r = ar::e_roughness_axis(Real(0.09f), Real(0.09f));
    const Real e = ar::e_diel_lookup(*rig.kg.e_diel, Real(1), r, Real(1.5));
    ar::EonParams ep{};
    ep.base_color = ar::Spec{{Real(0.6f), Real(0.3f), Real(0.1f), Real(0.3f)}};
    ep.roughness = Real(0);
    const ar::Spec e_eon = ar::eon_albedo(ep, Real(1));
    const float spec[3] = {0.9f, 0.95f, 1.0f};
    double want[3];
    for (int c = 0; c < 3; ++c) {
      const double es = double(Real(spec[c]) * e);
      want[c] = double(e_eon.v[c]) * (1.0 - es) + es;
    }
    rig.run();
    for (uint32_t i = 0; i < kN; ++i) CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) != 0u);
    g[run] = read_guides(rig);
    for (uint32_t c = 0; c < 3; ++c) CHECK(g[run].albedo[c][0] == doctest::Approx(want[c]).epsilon(1e-6));
  }
  CHECK(g[0].albedo[0][0] > 0.0);
  for (uint32_t c = 0; c < 3; ++c)
    for (uint32_t p = 0; p < kN; ++p) {
      CHECK(g[0].albedo[c][p] == g[1].albedo[c][p]);
      CHECK(g[0].albedo[c][p] == g[0].albedo[c][0]);  // a constant plane: one value per channel
      CHECK(g[0].normal[c][p] == g[1].normal[c][p]);
      CHECK(g[0].normal[c][p] == (c == 2 ? 1.0 : 0.0));
    }
}

TEST_CASE("shade_surface_guides/mirror follows tint") {
  // A delta metal (base_metalness 1, roughness 0) in front of a diffuse wall: the mirror's vertex
  // only multiplies the tint by its RGB albedo F82(mu) E_spec(mu, 0) c, and the wall writes its
  // albedo times that tint, its normal, and the bit.
  constexpr uint32_t kN = 16;
  Rig rig(kN, true);
  rig.kg.session.seed = 5u;
  ar::ConstantBlock& m = rig.block(0);
  m.base_weight = 1.0f, m.base_metalness = 1.0f, m.specular_weight = 1.0f, m.specular_roughness = 0.0f;
  m.emission_luminance = 0.0f;
  rig.base_color(0, 1.0f, 0.78f, 0.34f);
  rig.specular_color(0, 1.0f, 1.0f, 1.0f);
  rig.diffuse_wall();
  std::mt19937_64 rng(3);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  // The tint from the metal lobe's own functions in RGB lanes (metal.h, tables.h).
  ar::MetalParams mp{};
  for (int l = 0; l < ar::kLanes; ++l) {
    const float rgb[4] = {1.0f, 0.78f, 0.34f, 0.78f};
    mp.base_color.v[l] = Real(rgb[l]), mp.specular_color.v[l] = Real(1);
  }
  mp.base_weight = Real(1), mp.specular_weight = Real(1), mp.roughness = Real(0);
  const Real e_m = ar::e_spec_lookup(*rig.kg.e_spec, Real(1), ar::e_roughness_axis(Real(0), Real(0)));
  const ar::Spec f = ar::metal_fresnel(mp, Real(1)), c = ar::metal_compensation(mp, e_m);
  const double want_tint[3] = {double(f.v[0] * e_m * c.v[0]), double(f.v[1] * e_m * c.v[1]), double(f.v[2] * e_m * c.v[2])};
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) {
    REQUIRE(rig.term[i] == uint8_t(ar::Term::ACTIVE));
    CHECK((rig.flags[i] & ar::LAST_BOUNCE_SPECULAR) != 0u);
    CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) == 0u);
    for (uint32_t k = 0; k < 3; ++k) CHECK(comp(rig.tint[i], k) == doctest::Approx(want_tint[k]).epsilon(1e-6));
  }
  rig.to_wall();
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) != 0u);
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p)
    for (uint32_t k = 0; k < 3; ++k) {
      CHECK(g.albedo[k][p] == doctest::Approx(kWallAlbedo[k] * want_tint[k]).epsilon(1e-6));
      CHECK(g.normal[k][p] == (k == 2 ? -1.0 : 0.0));  // the wall faces the incoming ray
    }
}

TEST_CASE("shade_surface_guides/glass tint") {
  // A smooth dielectric with no diffuse base: every sample is the delta reflection, whose RGB albedo
  // is specular_color E_diel(mu, 0, 1.5); the wall's albedo carries it.
  constexpr uint32_t kN = 16;
  Rig rig(kN, true);
  rig.kg.session.seed = 9u;
  ar::ConstantBlock& gl = rig.block(0);
  gl.base_weight = 0.0f, gl.base_metalness = 0.0f, gl.specular_weight = 1.0f, gl.specular_roughness = 0.0f;
  gl.specular_ior = ar::make_const_ref(ar::SpectrumSemantic::IOR, Real(1.5));
  gl.emission_luminance = 0.0f;
  rig.specular_color(0, 0.9f, 0.8f, 0.7f);
  rig.diffuse_wall();
  const Real3 d = ar::normalize(Real3{Real(0.25), Real(0), Real(-1)});
  std::mt19937_64 rng(4);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), d, rng);
  const Real e = ar::e_diel_lookup(*rig.kg.e_diel, -d.z, ar::e_roughness_axis(Real(0), Real(0)), Real(1.5));
  const double want_tint[3] = {0.9 * double(e), 0.8 * double(e), 0.7 * double(e)};
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) {
    REQUIRE(rig.term[i] == uint8_t(ar::Term::ACTIVE));
    CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) == 0u);
    for (uint32_t k = 0; k < 3; ++k) CHECK(comp(rig.tint[i], k) == doctest::Approx(want_tint[k]).epsilon(1e-6));
  }
  rig.to_wall();
  rig.run();
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p)
    for (uint32_t k = 0; k < 3; ++k) CHECK(g.albedo[k][p] == doctest::Approx(kWallAlbedo[k] * want_tint[k]).epsilon(1e-6));
}

TEST_CASE("shade_surface_guides/escape writes the background value") {
  // The mirror's reflection escapes to a constant world: the albedo guide is tint x the escaped
  // contribution in display RGB at the sample's film wavelengths, over s (Cycles), the normal 0.
  constexpr uint32_t kN = 16;
  Rig rig(kN, true);
  rig.kg.session.seed = 13u;
  ar::ConstantBlock& m = rig.block(0);
  m.base_weight = 1.0f, m.base_metalness = 1.0f, m.specular_weight = 1.0f, m.specular_roughness = 0.0f;
  m.emission_luminance = 0.0f;
  rig.base_color(0, 1.0f, 0.78f, 0.34f);
  std::mt19937_64 rng(6);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  rig.run();
  std::vector<Real3> tint = rig.tint;
  // The world, for shade_background only.
  std::vector<ar::LightRecord> lights{ar::light_make_world(Real(3), ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, Real(1)))};
  ar::LightSamplerView sampler{};
  sampler.light_count = 1;
  rig.kg.lights = lights.data();
  rig.kg.world_light = 0;
  rig.kg.light_sampler = &sampler;
  for (uint32_t i = 0; i < kN; ++i) {
    REQUIRE(rig.next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    rig.next_stage[i] = uint8_t(ar::Stage::SHADE_BACKGROUND);
  }
  cpu_stage_shade_background(rig.kg, ar::StepEntry{0, kN, 0}, 0, kN);
  const ar::FilmData& f = rig.film->data();
  std::vector<double> want[3];
  for (uint32_t i = 0; i < kN; ++i) {
    CHECK(rig.term[i] == uint8_t(ar::Term::ESCAPED));
    ar::Spec L;
    for (int l = 0; l < ar::kLanes; ++l) L.v[l] = Real(f.slot_L[4u * i + uint32_t(l)]);
    REQUIRE(ar::max_lane(L) > Real(0));
    const ar::FilmWavelengths fw = ar::camera_film_wavelengths(rig.kg.session, f.wavelengths, i, 0u);
    const ar::DisplayRgb rgb = ar::display_rgb(f.response, L, fw.lf, ar::film_weights(f.slot_flags[i], fw.pf), f.film_exposure);
    for (uint32_t k = 0; k < 3; ++k) want[k].push_back(comp(tint[i], k) * rgb.v[k] * f.inv_radiance_scale);
  }
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p)
    for (uint32_t k = 0; k < 3; ++k) {
      // The stage converts in float, the expectation in double: 0002 D12's float-against-double
      // evaluation threshold (agree.h PROD_VS_CPU_F64).
      CHECK(g.albedo[k][p] == doctest::Approx(want[k][p]).epsilon(astroray::agree::PROD_VS_CPU_F64.eval_p999));
      CHECK(g.normal[k][p] == 0.0);
    }
}

TEST_CASE("shade_surface_guides/off writes nothing") {
  // No guide planes: the stage neither writes a guide nor touches the tint or the bit.
  constexpr uint32_t kN = 16;
  Rig rig(kN, false);
  CHECK(rig.film->layout().find("guide_albedo") == nullptr);
  CHECK(rig.film->layout().find("guide_normal") == nullptr);
  CHECK(rig.film->data().guide_albedo == -1);
  CHECK(rig.film->data().guide_normal == -1);
  ar::ConstantBlock& m = rig.block(0);
  m.base_weight = 1.0f, m.base_metalness = 1.0f, m.specular_weight = 1.0f, m.specular_roughness = 0.0f;
  m.emission_luminance = 0.0f;
  rig.base_color(0, 1.0f, 0.78f, 0.34f);
  rig.diffuse_wall();
  std::mt19937_64 rng(8);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  rig.run();
  rig.to_wall();
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) {
    CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) == 0u);
    CHECK(rig.tint[i].x == Real(-7));
    CHECK(rig.tint[i].y == Real(-7));
    CHECK(rig.tint[i].z == Real(-7));
  }
}

TEST_CASE("shade_surface_guides/per-point colour is magenta") {
  // base_color set per point (a UV-driven colour, the shape of a textured plane): the compiler marks
  // the block and the hit writes the unsupported colour (1, 0, 1) as its albedo.
  std::vector<ar_graph_node> nodes(2);
  std::vector<ar_graph_link> links{{0, 0, 1, 0}};
  std::vector<int32_t> ip{0, int32_t(ar::RecordSlot::base_color)};
  nodes[0].op = uint32_t(ar::Op::LOAD_CTX_UV), nodes[0].i32_first = 0, nodes[0].i32_count = 1;
  nodes[1].op = uint32_t(ar::Op::STORE_SLOT), nodes[1].i32_first = 1, nodes[1].i32_count = 1;
  ar_graph_desc d = {};
  d.node_count = 2, d.link_count = 1, d.i32_count = 2, d.nodes = nodes.data(), d.links = links.data(), d.params_i32 = ip.data();
  constexpr uint32_t kN = 8;
  Rig rig(kN, true, &d);
  REQUIRE((rig.block(0).guide_flags & ar::GUIDE_RGB_RUNTIME) != 0u);
  std::mt19937_64 rng(10);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) != 0u);
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p) {
    CHECK(g.albedo[0][p] == 1.0);
    CHECK(g.albedo[1][p] == 0.0);
    CHECK(g.albedo[2][p] == 1.0);
  }
}

TEST_CASE("shade_surface_guides/cauchy index uses n_d") {
  // A dispersive index (Cauchy, n_d 1.7): the record's lanes differ per wavelength, the guide takes
  // n_d, so the tint is specular_color E_diel(mu, 0, 1.7) whatever the lanes.
  constexpr uint32_t kN = 16;
  Rig rig(kN, true);
  rig.kg.session.seed = 15u;
  ar::ConstantBlock& gl = rig.block(0);
  gl.base_weight = 0.0f, gl.base_metalness = 0.0f, gl.specular_weight = 1.0f, gl.specular_roughness = 0.0f;
  gl.specular_ior = ar::SpectrumRef{};
  gl.specular_ior.kind = uint8_t(ar::SpectrumKind::CAUCHY_ABBE);
  gl.specular_ior.semantic = uint8_t(ar::SpectrumSemantic::IOR);
  gl.specular_ior.p[0] = 1.7f, gl.specular_ior.p[1] = 30.0f, gl.specular_ior.p[2] = 1.0f;
  gl.emission_luminance = 0.0f;
  rig.specular_color(0, 0.9f, 0.8f, 0.7f);
  const Real3 d = ar::normalize(Real3{Real(0.25), Real(0), Real(-1)});
  std::mt19937_64 rng(16);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), d, rng);
  const Real e = ar::e_diel_lookup(*rig.kg.e_diel, -d.z, ar::e_roughness_axis(Real(0), Real(0)), Real(1.7f));
  const double want_tint[3] = {0.9 * double(e), 0.8 * double(e), 0.7 * double(e)};
  rig.run();
  for (uint32_t i = 0; i < kN; ++i) {
    REQUIRE(rig.term[i] == uint8_t(ar::Term::ACTIVE));
    for (uint32_t k = 0; k < 3; ++k) CHECK(comp(rig.tint[i], k) == doctest::Approx(want_tint[k]).epsilon(1e-6));
  }
}

TEST_CASE("shade_surface_guides/escape at the first segment") {
  // A camera ray that escapes with the bit clear writes the background value at tint 1 into its
  // sample's pixel; the pool slots hold the samples in reverse, so a write by pool slot would show.
  constexpr uint32_t kN = 16;
  Rig rig(kN, true);
  rig.kg.session.seed = 17u;
  std::mt19937_64 rng(18);
  for (uint32_t i = 0; i < kN; ++i) {
    rig.start(i, 0, front_point(i, kN), Real3{0, 0, 1}, rng);
    rig.sample_id[i] = kN - 1u - i;
    rig.next_stage[i] = uint8_t(ar::Stage::SHADE_BACKGROUND);
  }
  const ar::SpectrumRef c = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, Real(1));
  std::vector<ar::LightRecord> lights{ar::light_make_world(Real(2), c)};
  ar::LightSamplerView sampler{};
  sampler.light_count = 1;
  rig.kg.lights = lights.data();
  rig.kg.world_light = 0;
  rig.kg.light_sampler = &sampler;
  cpu_stage_shade_background(rig.kg, ar::StepEntry{0, kN, 0}, 0, kN);
  const ar::FilmData& f = rig.film->data();
  std::vector<double> want[3];
  for (uint32_t s = 0; s < kN; ++s) {  // by sample, which is the pixel here
    ar::Spec L;
    for (int l = 0; l < ar::kLanes; ++l) L.v[l] = Real(f.slot_L[4u * s + uint32_t(l)]);
    REQUIRE(ar::max_lane(L) > Real(0));
    const ar::FilmWavelengths fw = ar::camera_film_wavelengths(rig.kg.session, f.wavelengths, s, 0u);
    const ar::DisplayRgb rgb = ar::display_rgb(f.response, L, fw.lf, ar::film_weights(f.slot_flags[s], fw.pf), f.film_exposure);
    for (uint32_t k = 0; k < 3; ++k) want[k].push_back(rgb.v[k] * f.inv_radiance_scale);
  }
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p)
    for (uint32_t k = 0; k < 3; ++k) {
      CHECK(g.albedo[k][p] == doctest::Approx(want[k][p]).epsilon(astroray::agree::PROD_VS_CPU_F64.eval_p999));
      CHECK(g.normal[k][p] == 0.0);
    }
}

TEST_CASE("shade_surface_guides/an escape guide that overflows fails the sample") {
  // A finite contribution near the float limit overflows the float display conversion (film weights
  // of about 160 here): the guide is not finite, so the sample is invalid, as for a non-finite
  // contribution, and nothing is written.
  constexpr uint32_t kN = 4;
  Rig rig(kN, true);
  std::mt19937_64 rng(22);
  for (uint32_t i = 0; i < kN; ++i) {
    rig.start(i, 0, front_point(i, kN), Real3{0, 0, 1}, rng);
    rig.next_stage[i] = uint8_t(ar::Stage::SHADE_BACKGROUND);
  }
  std::vector<ar::LightRecord> lights{ar::light_make_world(Real(2.0e38f), ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, Real(1)))};
  ar::LightSamplerView sampler{};
  sampler.light_count = 1;
  rig.kg.lights = lights.data();
  rig.kg.world_light = 0;
  rig.kg.light_sampler = &sampler;
  cpu_stage_shade_background(rig.kg, ar::StepEntry{0, kN, 0}, 0, kN);
  const ar::FilmData& f = rig.film->data();
  for (uint32_t i = 0; i < kN; ++i) {
    CHECK(rig.term[i] == uint8_t(ar::Term::INVALID));
    CHECK(std::isnan(f.slot_L[4u * i]));
    for (uint32_t k = 0; k < 6; ++k) CHECK(f.add[uint64_t(uint32_t(f.guide_albedo) + k) * f.plane_stride + i] == 0.0f);
  }
}

TEST_CASE("shade_surface_guides/a set bit writes nothing") {
  // A path whose bit is set (a volume scatter before any surface, shade_volume.h) writes no guide at
  // a later diffuse surface and keeps its tint.
  constexpr uint32_t kN = 8;
  Rig rig(kN, true);
  rig.diffuse_wall();
  std::mt19937_64 rng(20);
  for (uint32_t i = 0; i < kN; ++i) {
    rig.start(i, 1, front_point(i, kN) + Real3{0, 0, 2}, Real3{0, 0, 1}, rng);
    rig.flags[i] |= ar::DENOISE_SURFACE_DONE;
    rig.bounces[i].v[0] = rig.bounces[i].v[4] = 1;
  }
  rig.run();
  const Guides g = read_guides(rig);
  for (uint32_t p = 0; p < kN; ++p) {
    CHECK(rig.tint[p].x == Real(1));
    for (uint32_t k = 0; k < 3; ++k) {
      CHECK(g.albedo[k][p] == 0.0);
      CHECK(g.normal[k][p] == 0.0);
    }
  }
}

TEST_CASE("shade_surface_guides/film requests") {
  // The guide planes come as a pair with FilmRequest::guides and never as extra channels.
  FilmRequest req;
  req.max_width = req.max_height = 2;
  CHECK_FALSE(FilmLayout::build(req).guides());
  req.guides = true;
  CHECK(FilmLayout::build(req).guides());
  req.guides = false;
  req.extra.push_back({"guide_albedo", ChannelKind::Add, 3, 0});
  CHECK_THROWS_AS(FilmLayout::build(req), std::invalid_argument);
  req.extra[0].name = "guide_normal";
  CHECK_THROWS_AS(FilmLayout::build(req), std::invalid_argument);
}

}  // TEST_SUITE("fast")
