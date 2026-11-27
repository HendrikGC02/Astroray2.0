// SPDX-License-Identifier: Apache-2.0
// Alpha pass-through in shade_surface: hand-built hits on a real scene's tables (two quads facing
// +z at z = 0 and z = -0.5, one material whose constant block the test replaces). The opacity
// decision on the event's lobe-choice uniform (u >= a passes, u < a takes the surface with u / a,
// 0004 D14), the null event (only the transparent counter and the event index advance, the ray
// leaves from just past the hit, prev_dist keeps the previous vertex; 0004 D3, 0002 D11), the
// transparent limit (the hit adds its emission, then ends TRANSPARENT_LIMIT), the order against
// the total limit, and, in the gate tier, the pass frequency and ten stacked cards at 0.5^10.
// Ref: docs/decisions/0004-integrator.md D3, D14 and the D3/D14 Verification row; 0005 items 4,
// 38; 0003 d19, d23; pbrt-v4 src/pbrt/util/sampling.h SampleDiscrete.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/film/film_layout.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_surface.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr ar_key kMesh = 100, kMat = 1;
constexpr uint32_t kMaxBounces = 8, kTransparent = 4;

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
  ar::LightSamplerView no_lights{};  // no NEE: these cases test the decision and the null event
  uint32_t n;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;

  // `aov`: the film holds the primary-hit AOV planes.
  explicit Rig(uint32_t count, bool aov = false) : n(count) {
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
      REQUIRE(scene.materials->set_graph(e, kMat + 1000, &d) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = kMat + 1000;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, kMat, &m) == AR_OK);
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
      const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
      ar_mesh_desc md = {};
      md.key = kMesh, md.vertex_count = 4, md.corner_count = 4, md.face_count = 1, md.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &md, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(kMesh) == AR_OK);
    });
    commit([&](Edit& e) {
      // Two instances of the quad: at z = 0 and at z = -0.5.
      std::vector<ar_key> geom{kMesh, kMesh}, object{500, 501}, keys{kMat, kMat};
      std::vector<ar_xform> xf(2);
      for (ar_xform& x : xf) x.m[0] = x.m[5] = x.m[10] = 1.0f;
      xf[1].m[11] = -0.5f;
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
    req.statistics = true;
    req.termination_stats = true;
    req.primary_aov = aov;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(ar::LAST_BOUNCE_SPECULAR, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u);
      event_index.push_back(0);
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{ar::Real(0.25), ar::Real(0.25)});
      hit_t.push_back(ar::Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1});
      ray_d.push_back(ar::Real3{ar::Real(0.1), ar::Real(-0.2), ar::Real(-1)});
      eta_scale.push_back(ar::Real(1));
      prev_ng.push_back(0x1234u), prev_ns.push_back(0x5678u), prev_dist.push_back(ar::Real(7));
      ar::Spec lt, b, ru, rl;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = ar::Real(430 + 70 * l);
        b.v[l] = ar::Real(0.5 + 0.125 * double(l));
        ru.v[l] = ar::Real(1.0 + 0.25 * double(l));
        rl.v[l] = ar::Real(2.0 + 0.5 * double(l));
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(rl);
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 23;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = kMaxBounces;
    in.uniforms.max_bounces[5] = kTransparent;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
    kg.light_sampler = &no_lights;
    sd = *kg.scene;
    blocks.assign(sd.material_constants, sd.material_constants + kg.material_constant_count);
    sd.material_constants = blocks.data();
    kg.scene = &sd;
    // A Lambertian base of albedo 0.5 with the given opacity and no emission.
    ar::ConstantBlock& bl = block();
    bl.base_weight = 1.0f, bl.base_color = grey(0.5f), bl.base_diffuse_roughness = 0.0f, bl.base_metalness = 0.0f;
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
  ar::ConstantBlock& block() { return blocks[sd.material_headers[sd.material_slots[0]].const_offset]; }
  static ar::SpectrumRef grey(float v) {
    ar::SpectrumRef r{};
    r.kind = uint8_t(ar::SpectrumKind::CONSTANT);
    r.p[0] = v;
    return r;
  }
  void opacity(float a) { block().geometry_opacity = a; }
  // Runs the stage over the first `count` entries of the queue.
  void run(uint32_t count) { cpu_stage_shade_surface(kg, ar::StepEntry{0, count, 0}, 0, count); }
  void run() { run(n); }
  void vertex(uint32_t s, ar::ShadeContext* c) { REQUIRE(ar::shade_context_build(kg, s, c)); }
  float lobe_u(uint32_t s) const {
    return float(ar::camera_path_draw(kg, s, ar::event_dim(event_index[s], ar::EV_LOBE_CHOICE)));
  }
  uint32_t hist(ar::Term t) const {
    const ar::FilmData& f = film->data();
    uint32_t total = 0;
    for (uint32_t p = 0; p < n; ++p) total += f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + p];
    return total;
  }
  float add_plane(int32_t first, uint32_t c, uint32_t pixel) const {
    const ar::FilmData& f = film->data();
    return f.add[uint64_t(uint32_t(first) + c) * f.plane_stride + pixel];
  }
  bool passed(uint32_t s, uint16_t transparent_before) const {
    return term[s] == uint8_t(ar::Term::ACTIVE) && bounces[s].v[0] == 0u && bounces[s].v[5] == transparent_before + 1u;
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("alpha_pass_through/opacity decision at the threshold") {
  // u >= a passes; u < a takes the surface and leaves u / a below 1 (SampleDiscrete).
  float u = std::nextafter(0.5f, 0.0f);
  CHECK(!ar::opacity_null_event(ar::Real(0.5), &u));
  CHECK(u == std::nextafter(0.5f, 0.0f) / 0.5f);
  CHECK(u < 1.0f);
  u = 0.5f;
  CHECK(ar::opacity_null_event(ar::Real(0.5), &u));
  CHECK(u == 0.5f);  // a null event leaves the uniform alone
  u = 0.15f;
  CHECK(!ar::opacity_null_event(ar::Real(0.3), &u));
  CHECK(u == 0.15f / 0.3f);
  // Opacity 1 never passes, 0 always does; outside [0, 1] it is clamped to OpenPBR's range.
  for (float v : {0.0f, 0.25f, 0.999f, ar::kBranchOneMinusEpsilon}) {
    float w = v;
    CHECK(!ar::opacity_null_event(ar::Real(1), &w));
    CHECK(w == v);
    w = v;
    CHECK(!ar::opacity_null_event(ar::Real(1.5), &w));
    w = v;
    CHECK(ar::opacity_null_event(ar::Real(0), &w));
    w = v;
    CHECK(ar::opacity_null_event(ar::Real(-0.5), &w));
  }
  // The rescaled uniform stays below 1 where the rounded division reaches it.
  u = std::nextafter(0.7f, 0.0f);
  CHECK(!ar::opacity_null_event(ar::Real(0.7), &u));
  CHECK(u < 1.0f);
}

TEST_CASE("alpha_pass_through/alpha 1 never passes and alpha 0 always does") {
  for (float a : {1.0f, 0.0f}) {
    INFO("opacity " << a);
    Rig rig(256);
    rig.opacity(a);
    rig.run();
    uint32_t passes = 0, surfaces = 0;
    for (uint32_t s = 0; s < rig.n; ++s) {
      passes += rig.passed(s, 0);
      surfaces += rig.bounces[s].v[0] == 1u || rig.term[s] != uint8_t(ar::Term::ACTIVE);
    }
    CHECK(passes == (a == 0.0f ? rig.n : 0u));
    CHECK(surfaces == (a == 0.0f ? 0u : rig.n));
  }
}

TEST_CASE("alpha_pass_through/null event keeps state") {
  Rig rig(4);
  rig.opacity(0.0f);
  for (uint32_t s = 0; s < rig.n; ++s) {
    rig.bounces[s] = ar::U16x6{{3, 1, 2, 0, 0, 1}};
    rig.event_index[s] = uint16_t(5 + s);
    if (s & 1u) rig.flags[s] |= ar::FRONT_FACE;  // the quad is hit from its front: a stale bit must stay
  }
  const Rig& c = rig;
  const std::vector<uint32_t> flags0 = c.flags, ng0 = c.prev_ng, ns0 = c.prev_ns;
  const std::vector<ar::Spec> beta0 = c.beta, ru0 = c.r_u, rl0 = c.r_l;
  const std::vector<ar::Real3> o0 = c.ray_o, d0 = c.ray_d;
  std::vector<ar::Real3> P(rig.n), Ng(rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    ar::ShadeContext ctx{};
    rig.vertex(s, &ctx);
    P[s] = ctx.P, Ng[s] = ctx.Ng;
  }
  rig.run();
  for (uint32_t s = 0; s < rig.n; ++s) {
    INFO(s);
    CHECK(rig.term[s] == uint8_t(ar::Term::ACTIVE));
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    for (int k = 0; k < 5; ++k) CHECK(rig.bounces[s].v[k] == ar::U16x6{{3, 1, 2, 0, 0, 1}}.v[k]);
    CHECK(rig.bounces[s].v[5] == 2u);
    CHECK(rig.event_index[s] == 6u + s);
    CHECK(rig.flags[s] == flags0[s]);
    CHECK(rig.ray_type[s] == 1u);
    CHECK(rig.prev_ng[s] == ng0[s]);
    CHECK(rig.prev_ns[s] == ns0[s]);
    CHECK(std::memcmp(&rig.beta[s], &beta0[s], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&rig.r_u[s], &ru0[s], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&rig.r_l[s], &rl0[s], sizeof(ar::Spec)) == 0);
    CHECK(std::memcmp(&rig.ray_d[s], &d0[s], sizeof(ar::Real3)) == 0);
    const ar::Real3 o = ar::offset_past_surface(P[s], Ng[s], d0[s]);
    CHECK(std::memcmp(&rig.ray_o[s], &o, sizeof(ar::Real3)) == 0);
    CHECK(rig.ray_o[s].z < 0.0f);  // past the quad, on the side the ray travels to
    const ar::Real moved = ar::dot(o - o0[s], d0[s]) / ar::dot(d0[s], d0[s]);
    CHECK(rig.prev_dist[s] == ar::Real(7) + moved);
    for (int l = 0; l < 4; ++l) CHECK(rig.film->data().slot_L[4u * s + uint32_t(l)] == 0.0f);
  }
}

TEST_CASE("alpha_pass_through/transparent limit ends the path") {
  // One below the limit the hit passes; at the limit the next hit adds its emission and ends
  // TRANSPARENT_LIMIT instead of passing (Blender: the next surface after the limit emits and ends).
  Rig rig(8);
  rig.opacity(0.0f);
  rig.block().emission_color = Rig::grey(1.0f);
  rig.block().emission_luminance = 1.0f;
  for (uint32_t s = 0; s < rig.n; ++s) rig.bounces[s].v[5] = uint16_t(kTransparent - 1u);
  rig.run();
  std::vector<float> first(rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    CHECK(rig.passed(s, uint16_t(kTransparent - 1u)));
    first[s] = rig.film->data().slot_L[4u * s];
    CHECK(first[s] > 0.0f);  // the passing hit's emission was added too
  }
  rig.run();
  CHECK(rig.hist(ar::Term::TRANSPARENT_LIMIT) == rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    CHECK(rig.bounces[s].v[5] == kTransparent);
    CHECK(rig.film->data().slot_L[4u * s] == 2.0f * first[s]);  // added again before the end
  }
}

TEST_CASE("alpha_pass_through/transparent limit stores this hit's flags") {
  // The path ends on the hit that meets the limit, so the flag word the film stores holds that hit's
  // side, not the previous surface's, and no AOV_WRITTEN (the decision comes before the AOVs). Slots
  // 0-1 arrive with FRONT_FACE clear at a front hit, slots 2-3 with it set at a back hit; every
  // other bit of the arriving word survives.
  Rig rig(4);
  rig.opacity(0.0f);
  for (uint32_t s = 0; s < rig.n; ++s) {
    rig.bounces[s].v[5] = uint16_t(kTransparent);
    if (s >= 2) {
      rig.ray_d[s] = ar::Real3{ar::Real(0.1), ar::Real(-0.2), ar::Real(1)};
      rig.flags[s] |= ar::FRONT_FACE;
    }
  }
  const std::vector<uint32_t> arrived = rig.flags;
  rig.run();
  CHECK(rig.hist(ar::Term::TRANSPARENT_LIMIT) == rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    const uint32_t stored = rig.film->data().slot_flags[s];
    CHECK(((stored & ar::FRONT_FACE) != 0u) == (s < 2));
    CHECK((stored & ar::AOV_WRITTEN) == 0u);
    CHECK((stored & ~ar::FRONT_FACE) == (arrived[s] & ~ar::FRONT_FACE));
  }
}

TEST_CASE("alpha_pass_through/a pass-through at the total limit passes") {
  // The decision comes before the total limit, so a null event at bounces[total] == limit passes.
  Rig rig(4);
  rig.opacity(0.0f);
  for (uint32_t s = 0; s < rig.n; ++s) rig.bounces[s].v[0] = uint16_t(kMaxBounces);
  rig.run();
  CHECK(rig.hist(ar::Term::MAX_BOUNCE) == 0u);
  for (uint32_t s = 0; s < rig.n; ++s) {
    CHECK(rig.term[s] == uint8_t(ar::Term::ACTIVE));
    CHECK(rig.bounces[s].v[5] == 1u);
  }
}

TEST_CASE("alpha_pass_through/prev_dist after two pass-throughs is the previous vertex") {
  // A ray straight down from vertex V passes the quad at z = 0, then the one at z = -0.5; after
  // both, ray_o - prev_dist ray_d is V again (0003 d19).
  Rig rig(1);
  rig.opacity(0.0f);
  ar::ShadeContext c{};
  rig.ray_d[0] = ar::Real3{0, 0, -1};
  rig.vertex(0, &c);
  const ar::Real3 V = c.P + ar::Real3{0, 0, ar::Real(1.25)};
  rig.ray_o[0] = V, rig.prev_dist[0] = ar::Real(0);
  rig.run();
  REQUIRE(rig.passed(0, 0));
  rig.hit_inst[0] = 1u;  // the same barycentrics on the lower quad: the same x, y on the ray
  rig.run();
  REQUIRE(rig.passed(0, 1));
  const ar::Real3 back = rig.ray_o[0] - rig.ray_d[0] * rig.prev_dist[0];
  CHECK(std::fabs(double(back.x - V.x)) < 1e-6);
  CHECK(std::fabs(double(back.y - V.y)) < 1e-6);
  CHECK(std::fabs(double(back.z - V.z)) < 1e-5);
  CHECK(rig.ray_o[0].z < -0.5f);
}

TEST_CASE("alpha_pass_through/the surface takes u over a for its lobe choice") {
  // Opacity 0.5 over a dark base under a full-weight specular layer, so the stack picks its lobe
  // with 0 < p_spec < 1: u below a gives the stack sample of u / a, u at or above a passes; one
  // uniform serves the decision and the lobe choice (0004 D14). The uniform only picks the lobe
  // (stack_sample with a separate uc), so the cases with u < p_spec <= u / a are the ones a raw u
  // would send to the other lobe; the test requires enough of them.
  Rig rig(256);
  rig.opacity(0.5f);
  rig.block().base_color = Rig::grey(0.1f);
  rig.block().specular_weight = 1.0f;
  std::vector<float> u(rig.n);
  std::vector<ar::LobeSample> want(rig.n);
  std::vector<char> other_lobe(rig.n, 0);
  for (uint32_t s = 0; s < rig.n; ++s) {
    u[s] = rig.lobe_u(s);
    ar::ShadeContext c{};
    rig.vertex(s, &c);
    ar::OpenPBRParams rec;
    ar::resolve_constant_record(rig.block(), c, rig.kg.tables->observer, rig.kg.scene->material_spectra, &rec);
    const ar::Frame frame = ar::frame_from_normal(c.N);
    const ar::Real p_spec = ar::stack_select(*rig.kg.e_diel, *rig.kg.e_spec, rec, ar::to_local(frame, c.wo).z).p_spec;
    REQUIRE(p_spec > ar::Real(0.1));
    REQUIRE(p_spec < ar::Real(0.9));
    const ar::Real2 u2{ar::camera_path_draw(rig.kg, s, ar::event_dim(0, ar::EV_BSDF_U)),
                       ar::camera_path_draw(rig.kg, s, ar::event_dim(0, ar::EV_BSDF_V))};
    want[s] = ar::stack_sample(*rig.kg.e_diel, *rig.kg.e_spec, rec, frame, c.wo, ar::Real(u[s] / 0.5f), u2, ar::TransportMode::Radiance);
    other_lobe[s] = u[s] < 0.5f && (ar::Real(u[s]) < p_spec) != (ar::Real(u[s] / 0.5f) < p_spec);
    if (other_lobe[s]) {
      const ar::LobeSample raw = ar::stack_sample(*rig.kg.e_diel, *rig.kg.e_spec, rec, frame, c.wo, ar::Real(u[s]), u2, ar::TransportMode::Radiance);
      CHECK(raw.flags != want[s].flags);  // the raw uniform names the other lobe
    }
  }
  rig.run();
  uint32_t below = 0, above = 0, discriminating = 0;
  for (uint32_t s = 0; s < rig.n; ++s) {
    INFO(s);
    if (u[s] >= 0.5f) {
      ++above;
      CHECK(rig.passed(s, 0));
      continue;
    }
    ++below;
    CHECK(rig.bounces[s].v[5] == 0u);
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE)) continue;  // a reflection below Ng or roulette
    discriminating += other_lobe[s];
    CHECK(rig.bounces[s].v[0] == 1u);
    CHECK(rig.ray_d[s].x == want[s].wi.x);
    CHECK(rig.ray_d[s].y == want[s].wi.y);
    CHECK(rig.ray_d[s].z == want[s].wi.z);
  }
  CHECK(below > rig.n / 4);
  CHECK(above > rig.n / 4);
  CHECK(discriminating > rig.n / 16);  // continued paths whose lobe a raw u would change
}

TEST_CASE("alpha_pass_through/primary AOVs name the surface behind a card") {
  // The AOV write comes after the opacity decision: a camera hit (total bounces 0) on a card it
  // passes writes no AOV and leaves AOV_WRITTEN clear, and the surface behind it is the one the
  // planes name.
  Rig rig(4, true);
  const ar::FilmData& f = rig.film->data();
  REQUIRE(f.aov_position >= 0);
  REQUIRE(f.aov_normal >= 0);
  REQUIRE(f.aov_uv >= 0);
  REQUIRE(f.aov_material >= 0);
  rig.opacity(0.0f);
  rig.run();
  for (uint32_t s = 0; s < rig.n; ++s) {
    INFO(s);
    REQUIRE(rig.passed(s, 0));
    CHECK((rig.flags[s] & ar::AOV_WRITTEN) == 0u);
    for (uint32_t c = 0; c < 3; ++c) {
      CHECK(rig.add_plane(f.aov_position, c, s) == 0.0f);
      CHECK(rig.add_plane(f.aov_normal, c, s) == 0.0f);
    }
    for (uint32_t c = 0; c < 2; ++c) CHECK(rig.add_plane(f.aov_uv, c, s) == 0.0f);
    CHECK(rig.add_plane(f.aov_material, 0, s) == 0.0f);
  }
  // The next hit: the opaque quad at z = -0.5 behind the card.
  rig.opacity(1.0f);
  std::vector<ar::Real3> P(rig.n);
  std::vector<ar::Real2> uv(rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    rig.hit_inst[s] = 1u;
    ar::ShadeContext c{};
    rig.vertex(s, &c);
    P[s] = c.P, uv[s] = c.uv[0];
  }
  rig.run();
  for (uint32_t s = 0; s < rig.n; ++s) {
    INFO(s);
    CHECK(rig.bounces[s].v[5] == 1u);
    CHECK((rig.flags[s] & ar::AOV_WRITTEN) != 0u);
    CHECK(rig.add_plane(f.aov_position, 0, s) == float(P[s].x));
    CHECK(rig.add_plane(f.aov_position, 1, s) == float(P[s].y));
    CHECK(rig.add_plane(f.aov_position, 2, s) == -0.5f);
    CHECK(rig.add_plane(f.aov_normal, 2, s) == 1.0f);
    CHECK(rig.add_plane(f.aov_uv, 0, s) == float(uv[s].x));
    CHECK(rig.add_plane(f.aov_uv, 1, s) == float(uv[s].y));
  }
}

}  // TEST_SUITE("fast")

TEST_SUITE("gate") {

TEST_CASE("alpha_pass_through/pass fraction") {
  // Over 1e5 independent uniforms the null-event fraction is 1 - a within 3 binomial sigma.
  constexpr uint32_t kN = 100000;
  constexpr double kA = 0.3;
  Rig rig(kN);
  rig.opacity(float(kA));
  rig.run();
  uint32_t passes = 0;
  for (uint32_t s = 0; s < kN; ++s) passes += rig.passed(s, 0);
  const double p = 1.0 - kA, sigma = std::sqrt(p * (1.0 - p) / kN);
  const double f = double(passes) / kN;
  MESSAGE("pass fraction " << f << ", expected " << p << " +- " << 3.0 * sigma);
  CHECK(std::fabs(f - p) <= 3.0 * sigma);
}

TEST_CASE("alpha_pass_through/ten cards frequency") {
  // Ten stacked cards of opacity 0.5: a path passes all ten with probability 0.5^10 when each card
  // draws from its own event block; reusing one uniform would give 0.5 (0004 D14). Each round runs
  // the stage on the paths that passed every card so far.
  constexpr uint32_t kN = 1u << 16, kCards = 10;
  Rig rig(kN);
  rig.opacity(0.5f);
  rig.kg.session.max_bounces[5] = 16;
  uint32_t alive = kN;
  for (uint32_t card = 0; card < kCards; ++card) {
    rig.run(alive);
    uint32_t k = 0;
    for (uint32_t i = 0; i < alive; ++i) {
      const uint32_t s = rig.queue[i];
      if (rig.passed(s, uint16_t(card))) rig.queue[k++] = s;
    }
    alive = k;
  }
  const double p = std::pow(0.5, double(kCards)), se = std::sqrt(double(kN) * p * (1.0 - p));
  MESSAGE("passed all ten cards: " << alive << ", expected " << double(kN) * p << " +- " << 3.0 * se);
  CHECK(std::fabs(double(alive) - double(kN) * p) <= 3.0 * se);
}

}  // TEST_SUITE("gate")
