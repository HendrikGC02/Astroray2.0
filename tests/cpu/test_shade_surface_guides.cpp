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
// nothing. Per-point colours: an image-textured plane's guide is each texel's RGB clamped to [0, 1]
// (as a textured specular_color's, through E_spec) and a Mix child's is its own RGB with a negative
// channel at exactly 0, all to the record's 10-bit packing; a colour from a lifted
// spectrum has no RGB and writes magenta, reported once. Per-pixel guide variance is printed.
// The film is read through FilmView after one resolve (each pixel holds one sample).
// Ref: docs/decisions/0008-film-outputs-denoise.md d15, d22, d23, V10; 0003 d21; Cycles
// kernel/film/denoising_passes.h and light_passes.h (behaviour, not pixels).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

constexpr ar_key kMesh = 100, kFront = 1, kWall = 2, kImage = 300;

// An image of linear float RGBA texels (PIXELS source, linear Rec.709), rows as stored.
struct TexImage {
  uint32_t w = 0, h = 0;
  std::vector<float> rgba;
};

// A surface graph built node by node (the shape test_shade_surface_agree.cpp's Graph has).
struct SurfaceGraph {
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
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  // STORE_SLOT of `src` into `slot`, in Mix child `child` when it is not negative.
  void store(int src, ar::RecordSlot slot, int32_t child = -1) {
    std::vector<int32_t> ip{int32_t(slot)};
    if (child >= 0) ip.push_back(child);
    link(src, add(ar::Op::STORE_SLOT, {}, ip), 0);
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

// The packed guide RGB's stated error (openpbr_params.h guide_rgb_pack): half a 10-bit step, plus
// float rounding in the albedo.
constexpr double kPackErr = 0.5 / 1023.0 + 1e-6;

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

  // `front` is the front quad's surface graph (null: an Emission graph whose block the test replaces);
  // `image`, when given, is image kImage, which that graph may sample.
  Rig(uint32_t count, bool guides, const ar_graph_desc* front = nullptr, const TexImage* image = nullptr)
      : n(count), guides_on(guides) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    // The image in its own generation, before the graph that samples it compiles.
    if (image != nullptr)
      commit([&](Edit& e) {
        ar_image_desc id = {};
        id.key = kImage, id.source = AR_IMAGE_SOURCE_PIXELS, id.color_interop = AR_COLOR_INTEROP_LIN_REC709;
        id.width = image->w, id.height = image->h;
        ar_image_buffers ib = {};
        REQUIRE(scene.materials->image_begin(e, &id, &ib) == AR_OK);
        REQUIRE(ib.pixels_count == image->rgba.size());
        std::memcpy(*ib.pixels, image->rgba.data(), image->rgba.size() * sizeof(float));
        REQUIRE(e.item_commit(kImage) == AR_OK);
      });
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

// 8 x 8 float texels in distinct colours, red and green up to 1.35 so that some clamp (an image refuses a\n// negative texel, so the lower clamp has no texture case).
TexImage test_image() {
  TexImage im;
  im.w = im.h = 8;
  for (uint32_t i = 0; i < im.w * im.h; ++i) {
    im.rgba.push_back(1.35f * float((i * 37u) % 64u) / 63.0f);
    im.rgba.push_back(0.02f + 1.3f * float((i * 11u + 5u) % 64u) / 63.0f);
    im.rgba.push_back(0.03f + 0.9f * float((i * 23u + 3u) % 64u) / 63.0f);
    im.rgba.push_back(1.0f);
  }
  return im;
}

// The front quad's point at the centre of texel (tx, ty): the quad's UV is ((x + 1) / 2, (y + 1) / 2).
Real3 texel_centre(const TexImage& im, uint32_t tx, uint32_t ty) {
  const double u = (double(tx) + 0.5) / double(im.w), v = (double(ty) + 0.5) / double(im.h);
  return Real3{Real(2.0 * u - 1.0), Real(2.0 * v - 1.0), Real(0)};
}

// A diffuse material (specular_weight 0) whose base_color is image kImage, Closest, at UV set 0.
SurfaceGraph textured_graph() {
  SurfaceGraph g;
  const int tex = g.add(ar::Op::TEX_IMAGE, {}, {int32_t(kImage), 0});
  g.link(g.add(ar::Op::LOAD_CTX_UV, {}, {0}), tex, 0);
  g.store(tex, ar::RecordSlot::base_color);
  g.store(g.add(ar::Op::CONST_F32, {0.0f}), ar::RecordSlot::specular_weight);
  return g;
}

// Mix(factor) of two diffuse children whose base colours are c0 and c1.
SurfaceGraph mix_graph(const float* c0, const float* c1, float factor) {
  SurfaceGraph g;
  for (int32_t child = 0; child < 2; ++child) {
    const float* c = child == 0 ? c0 : c1;
    g.store(g.add(ar::Op::CONST_F32X3, {c[0], c[1], c[2]}), ar::RecordSlot::base_color, child);
    g.store(g.add(ar::Op::CONST_F32, {0.0f}), ar::RecordSlot::specular_weight, child);
  }
  g.link(g.add(ar::Op::CONST_F32, {factor}), g.add(ar::Op::BRANCH_MIX, {}, {0, 0, 1}), 0);
  return g;
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

TEST_CASE("shade_surface_guides/textured plane carries the texel rgb") {
  // A diffuse plane (no specular layer, EON at roughness 0, so the albedo is the base colour) whose
  // base_color is an 8 x 8 float image read Closest at UV set 0, one path at each texel's centre.
  // Texels reach above 1: each pixel's guide is the texel clamped to [0, 1], to the
  // packing's error, computed here from the texel values.
  const TexImage im = test_image();
  const SurfaceGraph gr = textured_graph();
  const ar_graph_desc d = gr.desc();
  const uint32_t n = im.w * im.h;
  Rig rig(n, true, &d, &im);
  REQUIRE(rig.block(0).guide_flags == 0u);
  std::mt19937_64 rng(30);
  for (uint32_t i = 0; i < n; ++i) rig.start(i, 0, texel_centre(im, i % im.w, i / im.w), Real3{0, 0, -1}, rng);
  rig.run();
  for (uint32_t i = 0; i < n; ++i) CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) != 0u);
  const Guides g = read_guides(rig);
  uint32_t clamped = 0;
  for (uint32_t p = 0; p < n; ++p)
    for (uint32_t c = 0; c < 3; ++c) {
      const double t = double(im.rgba[4 * p + c]);
      clamped += t < 0.0 || t > 1.0;
      CHECK(std::fabs(g.albedo[c][p] - std::min(std::max(t, 0.0), 1.0)) <= kPackErr);
    }
  CHECK(clamped > 0u);  // the clamp is exercised
}

TEST_CASE("shade_surface_guides/textured specular colour carries the texel rgb") {
  // A GGX layer (specular_weight 1, roughness 0.3) over EON with a constant base colour, its
  // specular_color the 8 x 8 image (Closest, UV set 0), one path at each texel's centre, straight down
  // (mu_o = 1). From the albedo tables directly (0008 d15): E_spec = s E_diel(1, r, 1.5) with s the
  // texel clamped to [0, 1], diffuse = E_eon (1 - E_spec), guide = diffuse + E_spec, computed here from
  // the texels. d(guide)/ds = E_diel (1 - E_eon) <= 1, so the packing's error bounds it.
  const TexImage im = test_image();
  SurfaceGraph gr;
  const float base[3] = {0.5f, 0.4f, 0.3f};
  gr.store(gr.add(ar::Op::CONST_F32X3, {base[0], base[1], base[2]}), ar::RecordSlot::base_color);
  const int tex = gr.add(ar::Op::TEX_IMAGE, {}, {int32_t(kImage), 0});
  gr.link(gr.add(ar::Op::LOAD_CTX_UV, {}, {0}), tex, 0);
  gr.store(tex, ar::RecordSlot::specular_color);
  const ar_graph_desc d = gr.desc();
  const uint32_t n = im.w * im.h;
  Rig rig(n, true, &d, &im);
  REQUIRE(rig.block(0).guide_flags == 0u);
  REQUIRE(rig.block(0).specular_weight == 1.0f);
  REQUIRE(rig.block(0).specular_roughness == 0.3f);
  std::mt19937_64 rng(34);
  for (uint32_t i = 0; i < n; ++i) rig.start(i, 0, texel_centre(im, i % im.w, i / im.w), Real3{0, 0, -1}, rng);
  const Real r = ar::e_roughness_axis(Real(0.09f), Real(0.09f));
  const double e = double(ar::e_diel_lookup(*rig.kg.e_diel, Real(1), r, Real(1.5)));
  ar::EonParams ep{};
  ep.base_color = ar::Spec{{Real(base[0]), Real(base[1]), Real(base[2]), Real(base[1])}};
  ep.roughness = Real(0);
  const ar::Spec e_eon = ar::eon_albedo(ep, Real(1));
  rig.run();
  for (uint32_t i = 0; i < n; ++i) CHECK((rig.flags[i] & ar::DENOISE_SURFACE_DONE) != 0u);
  const Guides g = read_guides(rig);
  uint32_t clamped = 0;
  for (uint32_t p = 0; p < n; ++p)
    for (uint32_t c = 0; c < 3; ++c) {
      const double t = double(im.rgba[4 * p + c]);
      clamped += t > 1.0;
      const double es = std::min(t, 1.0) * e;
      CHECK(std::fabs(g.albedo[c][p] - (double(e_eon.v[c]) * (1.0 - es) + es)) <= kPackErr);
    }
  CHECK(clamped > 0u);
}

TEST_CASE("shade_surface_guides/negative mix child channel is zero") {
  // A Mix child's constant colour reaches STORE_SLOT as code, unclamped: a negative channel is 0 in
  // the guide, exactly (the reflectance lift clamps it to 0 too, albedo_coeffs), the others the
  // child's RGB to the packing's error.
  const float c0[3] = {0.9f, -0.4f, 0.1f}, c1[3] = {-0.3f, 0.5f, 0.8f};
  const SurfaceGraph gr = mix_graph(c0, c1, 0.3f);
  const ar_graph_desc d = gr.desc();
  constexpr uint32_t kN = 64;
  Rig rig(kN, true, &d);
  std::mt19937_64 rng(36);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  rig.run();
  const Guides g = read_guides(rig);
  uint32_t first = 0, second = 0;
  for (uint32_t p = 0; p < kN; ++p) {
    // Which child: its positive channels match.
    const bool is0 = std::fabs(g.albedo[0][p] - double(c0[0])) <= kPackErr && std::fabs(g.albedo[2][p] - double(c0[2])) <= kPackErr;
    const bool is1 = std::fabs(g.albedo[1][p] - double(c1[1])) <= kPackErr && std::fabs(g.albedo[2][p] - double(c1[2])) <= kPackErr;
    REQUIRE((is0 || is1));
    CHECK(g.albedo[is0 ? 1 : 0][p] == 0.0);
    first += is0, second += is1;
  }
  CHECK(first > 0u);
  CHECK(second > 0u);
}

TEST_CASE("shade_surface_guides/mix child carries its rgb") {
  // Mix(0.3) of two diffuse children with constant colours, stored as code (CONST_STORE): each
  // sample's guide is the RGB of the child its selection took, and both children occur.
  const float c0[3] = {0.9f, 0.2f, 0.1f}, c1[3] = {0.1f, 0.5f, 0.8f};
  const SurfaceGraph gr = mix_graph(c0, c1, 0.3f);
  const ar_graph_desc d = gr.desc();
  constexpr uint32_t kN = 64;
  Rig rig(kN, true, &d);
  REQUIRE(rig.block(0).guide_flags == 0u);
  std::mt19937_64 rng(32);
  for (uint32_t i = 0; i < kN; ++i) rig.start(i, 0, front_point(i, kN), Real3{0, 0, -1}, rng);
  rig.run();
  const Guides g = read_guides(rig);
  uint32_t first = 0, second = 0;
  for (uint32_t p = 0; p < kN; ++p) {
    bool is0 = true, is1 = true;
    for (uint32_t c = 0; c < 3; ++c) {
      is0 = is0 && std::fabs(g.albedo[c][p] - double(c0[c])) <= kPackErr;
      is1 = is1 && std::fabs(g.albedo[c][p] - double(c1[c])) <= kPackErr;
    }
    CHECK((is0 || is1));
    first += is0, second += is1;
  }
  CHECK(first > 0u);
  CHECK(second > 0u);
}

TEST_CASE("shade_surface_guides/spectral only source stays magenta") {
  // base_color from a lifted spectrum (UV through RGB_LIFT): the record keeps no RGB, so the compiler
  // marks the block and reports it once, and the hit writes the unsupported colour (1, 0, 1).
  SurfaceGraph gr;
  const int lift = gr.add(ar::Op::RGB_LIFT, {}, {int32_t(ar::SpectrumSemantic::REFLECTANCE)});
  gr.link(gr.add(ar::Op::LOAD_CTX_UV, {}, {0}), lift, 0);
  gr.store(lift, ar::RecordSlot::base_color);
  const ar_graph_desc d = gr.desc();
  constexpr uint32_t kN = 8;
  Rig rig(kN, true, &d);
  REQUIRE((rig.block(0).guide_flags & ar::GUIDE_RGB_RUNTIME) != 0u);
  Report& r = rig.scene.report();
  r.snapshot();
  ar_report_info info{};
  REQUIRE(r.info(&info) == AR_OK);
  uint32_t entries = 0;
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    REQUIRE(r.entry(i, &e) == AR_OK);
    entries += e.code == AR_REPORT_NODE_APPROXIMATED && std::strstr(reinterpret_cast<const char*>(e.detail), "albedo guide") != nullptr;
  }
  CHECK(entries == 1u);
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

TEST_CASE("shade_surface_guides/per-pixel variance is reported") {
  // V10's per-pixel guide variance, reported and not gated: 16 pixels of 64 samples each, every path
  // its own film pixel and the samples grouped here. The textured plane's pixels are 1.5 texels wide,
  // so sub-pixel positions straddle texel edges; the Mix(0.3) pixels sit at fixed points, so only the
  // selection varies (expected p (1 - p) |c0 - c1|^2 per channel).
  constexpr uint32_t kPix = 16, kSpp = 64, kN = kPix * kSpp;
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  const auto variance = [&](const Guides& g, double out[3]) {
    for (uint32_t c = 0; c < 3; ++c) {
      double sum = 0.0;
      for (uint32_t p = 0; p < kPix; ++p) {
        double m = 0.0, m2 = 0.0;
        for (uint32_t s = 0; s < kSpp; ++s) {
          const double v = g.albedo[c][p * kSpp + s];
          REQUIRE(std::isfinite(v));
          m += v, m2 += v * v;
        }
        m /= kSpp;
        sum += (m2 / kSpp - m * m) * double(kSpp) / double(kSpp - 1);
      }
      out[c] = sum / kPix;
    }
  };
  double tex_var[3], mix_var[3];
  {
    const TexImage im = test_image();
    const SurfaceGraph gr = textured_graph();
    const ar_graph_desc d = gr.desc();
    Rig rig(kN, true, &d, &im);
    std::mt19937_64 rng(40);
    const double side = 1.5 / double(im.w);  // a pixel's width in UV
    for (uint32_t p = 0; p < kPix; ++p)
      for (uint32_t s = 0; s < kSpp; ++s) {
        const double u = (double(p % 4) + 0.5) / 4.0 + side * (uni(rng) - 0.5);
        const double v = (double(p / 4) + 0.5) / 4.0 + side * (uni(rng) - 0.5);
        rig.start(p * kSpp + s, 0, Real3{Real(2.0 * u - 1.0), Real(2.0 * v - 1.0), Real(0)}, Real3{0, 0, -1}, rng);
      }
    rig.run();
    variance(read_guides(rig), tex_var);
  }
  const float c0[3] = {0.9f, 0.2f, 0.1f}, c1[3] = {0.1f, 0.5f, 0.8f};
  {
    const SurfaceGraph gr = mix_graph(c0, c1, 0.3f);
    const ar_graph_desc d = gr.desc();
    Rig rig(kN, true, &d);
    std::mt19937_64 rng(41);
    for (uint32_t p = 0; p < kPix; ++p)
      for (uint32_t s = 0; s < kSpp; ++s) rig.start(p * kSpp + s, 0, front_point(p, kPix), Real3{0, 0, -1}, rng);
    rig.run();
    variance(read_guides(rig), mix_var);
  }
  std::printf("guide variance per pixel (mean over %u pixels of %u samples): textured plane %.4g %.4g %.4g; "
              "Mix(0.3) %.4g %.4g %.4g, binomial %.4g %.4g %.4g\n",
              kPix, kSpp, tex_var[0], tex_var[1], tex_var[2], mix_var[0], mix_var[1], mix_var[2],
              0.21 * double((c0[0] - c1[0]) * (c0[0] - c1[0])), 0.21 * double((c0[1] - c1[1]) * (c0[1] - c1[1])),
              0.21 * double((c0[2] - c1[2]) * (c0[2] - c1[2])));
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
