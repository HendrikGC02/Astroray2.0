// SPDX-License-Identifier: Apache-2.0
// The medium stack in shade_surface on hand-built hits (0006 D16-D17): at a medium-bearing instance's
// surface a false interface (the hit is not the highest priority) only updates the stack and the
// crossing counter, moves the origin past the surface and re-queues intersect_closest; a true interface
// changes the stack only where the path crosses (the opacity pass-through; a transmitting lobe and
// its crossing are M2's), so a reflection and its NEE item keep the stack the path arrived
// with. The scene's tables are real (four quads facing +z, instance i at z = -0.5 i with material
// i + 1); the media hang on copied instance records and the priorities on copied constant blocks, as
// the host sets DeviceInstanceRecord::medium and ConstantBlock::medium_priority.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D16, D17 (as implemented), V4 "stack
// overflow"; RULINGS 98; 0004 D3 (pass-through); Cycles kernel/integrator/path_state.h
// path_state_volume_next and kernel/types.h VOLUME_BOUNDS_MAX, read in
// third_party/_cache/refs/cycles-ed69ef2c6bd3.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <utility>
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
#include "kernel/stages/shade_surface.h"
#include "kernel/stages/route_hit.h"
#include "medium/medium_stack.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Real;
using ar::Spec;

namespace {

constexpr ar_key kMesh = 100;
constexpr uint32_t kInstances = 4, kMaxBounces = 8, kTransparent = 4;

// Hand-built camera paths on four instances of one quad. Every material starts as a Lambertian base of
// albedo 0.5, opaque, no emission, priority 0; no instance bears a medium. `lamp`: one point lamp
// above the quads, pmf 1, so a front-face hit emits a NEE item; otherwise no light and no NEE. `media`:
// the session's media feature with the path's and the shadow item's MEDIA fields allocated, as the host
// does; without it they are absent.
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
  std::vector<ar::DeviceInstanceRecord> instances;
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  uint32_t n;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns, medium_rng;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::U32x4> medium_stack;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<Real> hit_t, eta_scale, prev_dist;
  std::vector<Spec> lambda_t, beta, r_u, r_l;
  std::vector<uint32_t> sh_queue, sh_target, sh_rng;
  std::vector<uint8_t> sh_next, sh_flags;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<Real> sh_tmax;
  std::vector<Spec> sh_contrib, sh_lambda, sh_r_u, sh_r_l;
  std::vector<ar::U32x4> sh_stack;
  std::vector<ar::LightRecord> lamps;
  std::vector<ar::AliasBin> bins{{1.0f, 0u}};
  std::vector<uint32_t> bin_light{0u};
  std::vector<float> pmf{1.0f};
  ar::LightSamplerView sampler{};

  Rig(uint32_t count, bool lamp, bool media) : n(count) {
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
      REQUIRE(scene.materials->set_graph(e, 1000, &d) == AR_OK);
      for (uint32_t i = 0; i < kInstances; ++i) {
        ar_material_desc m = {};
        m.surface_graph = 1000;
        m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
        REQUIRE(scene.materials->set_material(e, ar_key(i + 1), &m) == AR_OK);
      }
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
      std::vector<ar_key> geom(kInstances, kMesh), object, keys;
      std::vector<ar_xform> xf(kInstances);
      std::vector<uint32_t> zeros(kInstances, 0), vis(kInstances, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff, mcount;
      for (uint32_t i = 0; i < kInstances; ++i) {
        object.push_back(500 + i), keys.push_back(ar_key(i + 1)), moff.push_back(i), mcount.push_back(1);
        xf[i].m[0] = xf[i].m[5] = xf[i].m[10] = 1.0f;
        xf[i].m[11] = -0.5f * float(i);
      }
      std::vector<ar_rgba> colors(kInstances, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(kInstances, 0);
      ar_instances_desc d = {};
      d.instance_count = kInstances, d.motion_steps = 1, d.transform_count = kInstances, d.material_key_count = kInstances;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);
    FilmRequest req;
    req.max_width = n, req.max_height = 1, req.slot_capacity = n;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0 / set->lights->radiance_scale;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(0u, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u), event_index.push_back(uint16_t(1 + i % 4));
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{Real(0.1 + 0.15 * (i % 4)), Real(0.2 + 0.1 * (i % 4))});
      hit_t.push_back(Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1});
      ray_d.push_back(ar::Real3{Real(0.1), Real(-0.2), Real(-1)});
      eta_scale.push_back(Real(1));
      prev_ng.push_back(0x1234u), prev_ns.push_back(0x5678u), prev_dist.push_back(Real(7));
      medium_stack.push_back(ar::U32x4{{0, 0, 0, 0}}), medium_rng.push_back(0x5EEDu);
      Spec lt, b, ru, rl;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = Real(430 + 70 * l + int(i % 7));
        b.v[l] = Real(0.5 + 0.125 * double(l));
        ru.v[l] = Real(1.0 + 0.25 * double(l) + 0.01 * double(i % 5));
        rl.v[l] = Real(2.0 + 0.5 * double(l));
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(rl);
      sh_queue.push_back(n - 1 - i), sh_target.push_back(~0u);
      sh_next.push_back(uint8_t(ar::Stage::SHADOW_TERMINATED));
      sh_o.push_back(ar::Real3{0, 0, 0}), sh_d.push_back(ar::Real3{0, 0, 0}), sh_tmax.push_back(Real(-1));
      sh_contrib.push_back(ar::spec_fill(0)), sh_flags.push_back(0xFFu), sh_rng.push_back(0xDEADBEEFu);
      sh_lambda.push_back(ar::spec_fill(-1)), sh_r_u.push_back(ar::spec_fill(-1)), sh_r_l.push_back(ar::spec_fill(-1));
      sh_stack.push_back(ar::U32x4{{0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu}});
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
    if (media) {
      path.medium_stack = medium_stack.data(), path.medium_rng = medium_rng.data();
      shadow.item_flags = sh_flags.data(), shadow.lambda_t = sh_lambda.data(), shadow.r_u = sh_r_u.data();
      shadow.r_l_film_pos = sh_r_l.data(), shadow.medium_stack = sh_stack.data(), shadow.medium_rng = sh_rng.data();
    }

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 29;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = kMaxBounces;
    in.uniforms.max_bounces[5] = kTransparent;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path, kg.shadow = &shadow, kg.main_queue = queue.data(), kg.shadow_queue = sh_queue.data();
    kg.session.features.media = media;
    sd = *kg.scene;
    blocks.assign(sd.material_constants, sd.material_constants + kg.material_constant_count);
    sd.material_constants = blocks.data();
    instances.assign(sd.instances, sd.instances + sd.instance_count);
    sd.instances = instances.data();
    sd.medium_count = kInstances;  // instance_medium only checks the index against the count
    kg.scene = &sd;
    if (lamp) {
      lamps = {ar::light_make(ar::LightType::POINT, ar::Real3{Real(0.3), Real(0.2), Real(2)}, Real(0), Real(10),
                              ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)))};
      sampler = ar::LightSamplerView{bins.data(), bin_light.data(), pmf.data(), 1u, 1u};
      kg.lights = lamps.data();
    }
    kg.light_sampler = &sampler;
    for (uint32_t i = 0; i < kInstances; ++i) {
      ar::ConstantBlock& bl = block(i);
      bl.base_weight = 1.0f, bl.base_color = grey(0.5f), bl.base_diffuse_roughness = 0.0f, bl.base_metalness = 0.0f;
      bl.specular_weight = 0.0f, bl.emission_luminance = 0.0f, bl.geometry_opacity = 1.0f, bl.medium_priority = 0;
    }
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
  static ar::SpectrumRef grey(float v) {
    ar::SpectrumRef r{};
    r.kind = uint8_t(ar::SpectrumKind::CONSTANT);
    r.p[0] = v;
    return r;
  }
  ar::ConstantBlock& block(uint32_t inst) {
    return blocks[sd.material_headers[sd.material_slots[instances[inst].material_offset]].const_offset];
  }
  // Instance `inst` bears medium index m (0-based), with its material's priority p.
  void medium(uint32_t inst, uint32_t m, uint8_t p) {
    instances[inst].medium = m + 1u;
    block(inst).medium_priority = p;
  }
  void opacity(uint32_t inst, float a) { block(inst).geometry_opacity = a; }
  // Slot s hits instance inst on its front (from above, going down) or its back (from below).
  void hit(uint32_t s, uint32_t inst, bool front) {
    hit_inst[s] = inst;
    ray_o[s] = ar::Real3{0, 0, front ? Real(1) : Real(-3)};
    ray_d[s] = ar::Real3{Real(0.1), Real(-0.2), front ? Real(-1) : Real(1)};
  }
  // The stack slot s arrived with, oldest first, as (medium index, priority) pairs.
  void arrive(uint32_t s, std::initializer_list<std::pair<uint32_t, uint32_t>> entries) {
    ar::U32x4 st{{0, 0, 0, 0}};
    uint32_t d = 0;
    for (const auto& e : entries) st.v[d++] = ar::stack_entry(e.first + 1u, e.second);
    medium_stack[s] = st;
    flags[s] = ar::set_medium_depth(flags[s], d);
  }
  // The hit point and its facing geometric normal, as the stage builds them.
  void vertex(uint32_t s, ar::Real3* P, ar::Real3* Ng) {
    ar::ShadeContext c{};
    REQUIRE(ar::shade_context_build(kg, s, &c));
    *P = c.P, *Ng = c.Ng;
  }
  void run(uint32_t count) { cpu_stage_shade_surface(kg, ar::StepEntry{0, count, 0}, 0, count); }
  void run() { run(n); }
  uint32_t hist(ar::Term t) const {
    const ar::FilmData& f = film->data();
    uint32_t total = 0;
    for (uint32_t p = 0; p < n; ++p) total += f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + p];
    return total;
  }
};

// FNV-1a 64 over bit patterns: a golden of every value the stage leaves, in one number.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t count) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < count; ++i) h = (h ^ b[i]) * 1099511628211ull;
  }
  template <class T>
  void vec(const std::vector<T>& v) {
    bytes(v.data(), v.size() * sizeof(T));
  }
};

// Every path, shadow-item and film value of the rig after a run (the MEDIA fields only when allocated).
uint64_t rig_digest(const Rig& r) {
  Digest d;
  d.vec(r.next_stage), d.vec(r.term), d.vec(r.ray_type), d.vec(r.flags), d.vec(r.bounces), d.vec(r.event_index);
  d.vec(r.ray_o), d.vec(r.ray_d), d.vec(r.beta), d.vec(r.r_u), d.vec(r.r_l), d.vec(r.prev_ng), d.vec(r.prev_ns);
  d.vec(r.prev_dist);
  d.vec(r.sh_next), d.vec(r.sh_o), d.vec(r.sh_d), d.vec(r.sh_tmax), d.vec(r.sh_contrib), d.vec(r.sh_target);
  if (r.path.medium_stack != nullptr) {
    d.vec(r.medium_stack), d.vec(r.medium_rng);
    d.vec(r.sh_flags), d.vec(r.sh_rng), d.vec(r.sh_lambda), d.vec(r.sh_r_u), d.vec(r.sh_r_l), d.vec(r.sh_stack);
  }
  const ar::FilmData& f = r.film->data();
  d.bytes(f.slot_L, size_t(4) * r.n * sizeof(float));
  d.bytes(f.slot_flags, size_t(r.n) * sizeof(uint32_t));
  for (uint32_t t = 0; t <= uint32_t(ar::Term::CANCELLED); ++t) {
    const uint32_t c = r.hist(ar::Term(t));
    d.bytes(&c, sizeof c);
  }
  return d.h;
}

// Three configurations of existing cases, none hitting a medium-bearing instance: an opacity-0.5 card
// with no light (test_alpha_pass_through's decision), opaque Lambertian hits from both sides under a
// point lamp (test_shade_shadow's emit rig), an emissive opacity-0 card at the transparent limit
// (test_alpha_pass_through's limit case). With `media`, instance 3 bears a medium that no slot hits.
uint64_t config_digest(int config, bool media) {
  const uint32_t n = 64;
  Rig r(n, config == 1, media);
  if (media) r.medium(3, 0, 5);
  for (uint32_t s = 0; s < n; ++s) r.hit(s, s % 3u, config == 1 ? (s & 1u) == 0u : true);
  if (config == 0) {
    for (uint32_t i = 0; i < 3; ++i) r.opacity(i, 0.5f);
  } else if (config == 2) {
    for (uint32_t i = 0; i < 3; ++i) {
      r.opacity(i, 0.0f);
      r.block(i).emission_color = Rig::grey(1.0f);
      r.block(i).emission_luminance = 1.0f;
    }
    for (uint32_t s = 0; s < n; ++s) r.bounces[s].v[5] = uint16_t(kTransparent - 1u + (s & 1u));
  }
  r.run();
  // The configurations reach what they stand for: passes and surfaces, NEE items, transparent-limit ends.
  uint32_t passed = 0, continued = 0, items = 0;
  for (uint32_t s = 0; s < n; ++s) {
    passed += r.bounces[s].v[5] > (config == 2 ? kTransparent - 1u + (s & 1u) : 0u);
    continued += r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST) && r.bounces[s].v[0] == 1u;
    items += r.sh_next[s] == uint8_t(ar::Stage::INTERSECT_SHADOW);
  }
  if (config == 0) CHECK((passed > 8u && continued > 8u));
  if (config == 1) CHECK((items > 8u && continued > 8u));
  if (config == 2) CHECK((passed > 8u && r.hist(ar::Term::TRANSPARENT_LIMIT) > 8u));
  return rig_digest(r);
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_surface_media/no medium is bit identical to the stage before the stack") {
  // Goldens: the digests of the three configurations with the media feature off and on, recorded from
  // the stage before the medium stack existed (an instance's medium was tested only at its own back face).
  const uint64_t golden[3][2] = {{0xfaa8ed417f8ddcc9ull, 0xaeb9501da0cde509ull},
                                 {0x984b3241c4371e1dull, 0x866eefea9bae5f2full},
                                 {0xea2fd382fd58cbe0ull, 0x6e7727c544d73ea0ull}};
  for (int c = 0; c < 3; ++c)
    for (int m = 0; m < 2; ++m) {
      const uint64_t got = config_digest(c, m != 0);
      INFO("config " << c << " media " << m << " digest " << got);
      CHECK(got == golden[c][m]);
    }
}

TEST_CASE("shade_surface_media/reflection does not enter") {
  // An opaque Lambertian box bearing a medium, hit on its front from outside: a true interface the
  // path does not cross. The continuation and the NEE item (lamp on the same side) keep the empty stack.
  Rig r(8, true, true);
  r.medium(0, 0, 0);
  for (uint32_t s = 0; s < r.n; ++s) r.hit(s, 0, true);
  r.run();
  uint32_t continued = 0, items = 0;
  for (uint32_t s = 0; s < r.n; ++s) {
    INFO("slot " << s);
    CHECK(ar::medium_depth(r.flags[s]) == 0u);
    CHECK(ar::crossings(r.flags[s]) == 0u);
    for (int k = 0; k < 4; ++k) CHECK(r.medium_stack[s].v[k] == 0u);
    if (r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST)) {
      ++continued;
      CHECK(r.bounces[s].v[0] == 1u);  // a real bounce
    }
    const uint32_t it = r.sh_queue[s];
    if (r.sh_next[it] != uint8_t(ar::Stage::INTERSECT_SHADOW)) continue;
    ++items;
    for (int k = 0; k < 4; ++k) CHECK(r.sh_stack[it].v[k] == 0u);
  }
  CHECK(continued > 0u);
  CHECK(items == r.n);
}

TEST_CASE("shade_surface_media/nee item takes the highest entry") {
  // A path inside two media, the higher-priority one entered first (medium 1 at priority 5, then medium 2 at
  // priority 0), reflects off an opaque surface without a medium under a point lamp: each NEE item names the
  // highest entry, 1 + 1, not the latest, 1 + 2 (0006 D17 active media; M1b tracks the highest).
  Rig r(8, true, true);
  for (uint32_t s = 0; s < r.n; ++s) r.hit(s, 0, true), r.arrive(s, {{1, 5}, {2, 0}});
  r.run();
  uint32_t items = 0;
  for (uint32_t s = 0; s < r.n; ++s) {
    const uint32_t it = r.sh_queue[s];
    if (r.sh_next[it] != uint8_t(ar::Stage::INTERSECT_SHADOW)) continue;
    ++items;
    INFO("slot " << s);
    CHECK(r.sh_stack[it].v[0] == 2u);
    for (int k = 1; k < 4; ++k) CHECK(r.sh_stack[it].v[k] == 0u);
  }
  CHECK(items == r.n);
}

TEST_CASE("shade_surface_media/enter and leave") {
  // One slab of opacity 0 bearing medium 0: the front-face pass-through pushes its entry (depth 1), the
  // back-face hit of the next segment goes to shade_volume first (depth 1) and then here, where the
  // pass-through removes it (depth 0). Each pass-through is a transparent bounce and a new event.
  Rig r(4, false, true);
  r.medium(0, 0, 7);
  r.opacity(0, 0.0f);
  for (uint32_t s = 0; s < r.n; ++s) r.hit(s, 0, true);
  r.run();
  for (uint32_t s = 0; s < r.n; ++s) {
    INFO("entering slot " << s);
    CHECK(r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(ar::medium_depth(r.flags[s]) == 1u);
    CHECK(r.medium_stack[s].v[0] == ar::stack_entry(1u, 7u));
    CHECK(r.bounces[s].v[5] == 1u);
    CHECK(r.event_index[s] == 2u + s % 4u);
    CHECK(ar::crossings(r.flags[s]) == 0u);
  }
  for (uint32_t s = 0; s < r.n; ++s) {
    r.hit(s, 0, false);
    const ar::HitRecord h{Real(1), 0u, 0u, Real(0.25), Real(0.25)};
    CHECK(ar::route_hit(r.kg, h, r.flags[s]) == ar::Stage::SHADE_VOLUME);
    CHECK(ar::route_hit_after_medium(r.kg, h, r.flags[s]) == ar::Stage::SHADE_SURFACE);
    r.next_stage[s] = uint8_t(ar::Stage::SHADE_SURFACE);
  }
  r.run();
  for (uint32_t s = 0; s < r.n; ++s) {
    INFO("leaving slot " << s);
    CHECK(r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(ar::medium_depth(r.flags[s]) == 0u);
    for (int k = 0; k < 4; ++k) CHECK(r.medium_stack[s].v[k] == 0u);
    CHECK(r.bounces[s].v[5] == 2u);
    const ar::HitRecord h{Real(1), 0u, 1u, Real(0.25), Real(0.25)};
    CHECK(ar::route_hit(r.kg, h, r.flags[s]) == ar::Stage::SHADE_SURFACE);
  }
}

TEST_CASE("shade_surface_media/nested priorities false interface") {
  // Slab A (instance 0, medium 0, priority 1) holds slab B (instance 1, medium 1, priority 2); both have
  // opacity 0 and A emits. B's faces are true interfaces; A's back face met inside B is false, and so is
  // A's front face met inside B alone.
  Rig r(4, false, true);
  r.medium(0, 0, 1), r.medium(1, 1, 2);
  r.opacity(0, 0.0f), r.opacity(1, 0.0f);
  r.block(0).emission_color = Rig::grey(1.0f), r.block(0).emission_luminance = 1.0f;
  r.arrive(0, {{0, 1}}), r.hit(0, 1, true);           // in A, entering B: true
  r.arrive(1, {{0, 1}, {1, 2}}), r.hit(1, 1, false);  // in A and B, leaving B: true
  r.arrive(2, {{0, 1}, {1, 2}}), r.hit(2, 0, false);  // in A and B, leaving A: false
  r.arrive(3, {{1, 2}}), r.hit(3, 0, true);           // in B, entering A: false
  for (uint32_t s = 0; s < r.n; ++s) r.bounces[s] = ar::U16x6{{2, 1, 1, 0, 0, 1}};
  const Rig& c = r;
  const std::vector<uint32_t> flags0 = c.flags, ng0 = c.prev_ng, ns0 = c.prev_ns;
  const std::vector<uint16_t> e0 = c.event_index;
  const std::vector<Spec> beta0 = c.beta, ru0 = c.r_u, rl0 = c.r_l;
  const std::vector<ar::Real3> o0 = c.ray_o, d0 = c.ray_d;
  std::vector<ar::Real3> P(r.n), Ng(r.n);
  for (uint32_t s = 0; s < r.n; ++s) r.vertex(s, &P[s], &Ng[s]);
  r.run();
  const ar::U32x4 want[4] = {{{ar::stack_entry(1, 1), ar::stack_entry(2, 2), 0, 0}},
                             {{ar::stack_entry(1, 1), 0, 0, 0}},
                             {{ar::stack_entry(2, 2), 0, 0, 0}},
                             {{ar::stack_entry(2, 2), ar::stack_entry(1, 1), 0, 0}}};
  for (uint32_t s = 0; s < r.n; ++s) {
    INFO("slot " << s);
    const bool false_interface = s >= 2;
    CHECK(r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(std::memcmp(&r.medium_stack[s], &want[s], sizeof(ar::U32x4)) == 0);
    CHECK(ar::medium_depth(r.flags[s]) == (s == 1 || s == 2 ? 1u : 2u));
    CHECK(ar::crossings(r.flags[s]) == (false_interface ? 1u : 0u));
    CHECK(r.bounces[s].v[5] == (false_interface ? 1u : 2u));  // a true pass-through is a transparent bounce
    CHECK(r.event_index[s] == (false_interface ? e0[s] : uint16_t(e0[s] + 1u)));
    if (!false_interface) continue;
    // Only the stack, the depth bits, the counter, the origin and prev_dist change.
    const uint32_t keep = ~(ar::MEDIUM_DEPTH_MASK | ar::CROSSINGS_MASK);
    CHECK((r.flags[s] & keep) == (flags0[s] & keep));
    for (int k = 0; k < 6; ++k) CHECK(r.bounces[s].v[k] == ar::U16x6{{2, 1, 1, 0, 0, 1}}.v[k]);
    CHECK(std::memcmp(&r.beta[s], &beta0[s], sizeof(Spec)) == 0);
    CHECK(std::memcmp(&r.r_u[s], &ru0[s], sizeof(Spec)) == 0);
    CHECK(std::memcmp(&r.r_l[s], &rl0[s], sizeof(Spec)) == 0);
    CHECK(std::memcmp(&r.ray_d[s], &d0[s], sizeof(ar::Real3)) == 0);
    CHECK(r.ray_type[s] == 1u);
    CHECK(r.prev_ng[s] == ng0[s]);
    CHECK(r.prev_ns[s] == ns0[s]);
    const ar::Real3 o = ar::offset_past_surface(P[s], Ng[s], d0[s]);
    CHECK(std::memcmp(&r.ray_o[s], &o, sizeof(ar::Real3)) == 0);
    CHECK(r.prev_dist[s] == Real(7) + ar::dot(o - o0[s], d0[s]) / ar::dot(d0[s], d0[s]));
    for (int l = 0; l < 4; ++l) CHECK(r.film->data().slot_L[4u * s + uint32_t(l)] == 0.0f);  // A's emission is not seen
  }
}

TEST_CASE("shade_surface_media/equal priorities latest wins") {
  // Two slabs at priority 0 (Blender's default): the most recently entered is the highest, so leaving
  // the older one inside the newer is false, and every entry is true.
  Rig r(4, false, true);
  r.medium(0, 0, 0), r.medium(1, 1, 0);
  r.opacity(0, 0.0f), r.opacity(1, 0.0f);
  r.arrive(0, {{0, 0}}), r.hit(0, 1, true);           // in A, entering B: true
  r.arrive(1, {{0, 0}, {1, 0}}), r.hit(1, 0, false);  // in A then B, leaving A: false
  r.arrive(2, {{0, 0}, {1, 0}}), r.hit(2, 1, false);  // in A then B, leaving B: true
  r.arrive(3, {{1, 0}, {0, 0}}), r.hit(3, 1, false);  // in B then A, leaving B: false
  r.run();
  const uint32_t want_crossings[4] = {0, 1, 0, 1}, want_depth[4] = {2, 1, 1, 1};
  const ar::U32x4 want[4] = {{{ar::stack_entry(1, 0), ar::stack_entry(2, 0), 0, 0}},
                             {{ar::stack_entry(2, 0), 0, 0, 0}},
                             {{ar::stack_entry(1, 0), 0, 0, 0}},
                             {{ar::stack_entry(1, 0), 0, 0, 0}}};
  for (uint32_t s = 0; s < r.n; ++s) {
    INFO("slot " << s);
    CHECK(r.next_stage[s] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
    CHECK(ar::crossings(r.flags[s]) == want_crossings[s]);
    CHECK(ar::medium_depth(r.flags[s]) == want_depth[s]);
    CHECK(std::memcmp(&r.medium_stack[s], &want[s], sizeof(ar::U32x4)) == 0);
  }
}

TEST_CASE("shade_surface_media/coincident shells end at the cap") {
  // A path inside slab A (priority 2) crosses slab B (priority 1) back and forth, as between two
  // coincident shells: every crossing is false, draws nothing and advances only the crossing count. The
  // crossing that would bring it to VOLUME_BOUNDS_MAX (1024) ends the path VOLUME_LIMIT, counted once.
  Rig r(1, false, true);
  r.medium(0, 0, 2), r.medium(1, 1, 1);
  r.arrive(0, {{0, 2}});
  uint32_t runs = 0;
  for (; runs < 4096u && r.term[0] == uint8_t(ar::Term::ACTIVE); ++runs) {
    r.hit(0, 1, (runs & 1u) == 0u);  // B's front, then its back
    r.next_stage[0] = uint8_t(ar::Stage::SHADE_SURFACE);
    r.run();
  }
  CHECK(runs == ar::kVolumeBoundsMax);
  CHECK(r.term[0] == uint8_t(ar::Term::VOLUME_LIMIT));
  CHECK(r.next_stage[0] == uint8_t(ar::Stage::TERMINATED));
  CHECK(r.hist(ar::Term::VOLUME_LIMIT) == 1u);
  CHECK(ar::crossings(r.flags[0]) == ar::kVolumeBoundsMax - 1u);
  CHECK(r.event_index[0] == 1u);  // no vertex in 1023 crossings
  CHECK(r.bounces[0].v[5] == 0u);
  CHECK(ar::medium_depth(r.flags[0]) == 2u);  // the last crossing passed B's front
}

TEST_CASE("shade_surface_media/overflow ends the path") {
  // Four slabs of opacity 0, each bearing its own medium at priority 0. A path entering them one after
  // another holds four entries; the fifth push (re-entering the first) ends it STACK_OVERFLOW with the
  // stack unchanged. A false interface's push past four entries ends the same way.
  Rig r(2, false, true);
  for (uint32_t i = 0; i < kInstances; ++i) r.medium(i, i, 0), r.opacity(i, 0.0f);
  r.kg.session.max_bounces[5] = 8;  // the transparent limit (4 in the rig) would end the fifth pass first
  r.arrive(1, {{1, 5}, {2, 5}, {3, 5}, {1, 5}});
  r.hit(1, 0, true);  // priority 0 below 5: false
  for (uint32_t k = 0; k < 5; ++k) {
    r.hit(0, k % kInstances, true);
    r.next_stage[0] = uint8_t(ar::Stage::SHADE_SURFACE);
    r.run(k == 0 ? 2u : 1u);
    INFO("push " << k + 1);
    CHECK(ar::medium_depth(r.flags[0]) == std::min(k + 1u, 4u));
    CHECK(r.term[0] == uint8_t(k < 4 ? ar::Term::ACTIVE : ar::Term::STACK_OVERFLOW));
  }
  const ar::U32x4 four{{ar::stack_entry(1, 0), ar::stack_entry(2, 0), ar::stack_entry(3, 0), ar::stack_entry(4, 0)}};
  CHECK(std::memcmp(&r.medium_stack[0], &four, sizeof(ar::U32x4)) == 0);
  CHECK(r.bounces[0].v[5] == 4u);
  CHECK(r.term[1] == uint8_t(ar::Term::STACK_OVERFLOW));
  CHECK(ar::medium_depth(r.flags[1]) == 4u);
  CHECK(ar::crossings(r.flags[1]) == 0u);
  CHECK(r.hist(ar::Term::STACK_OVERFLOW) == 2u);
}

TEST_CASE("shade_surface_media/back face with an absent id") {
  // A path that started inside a medium (until the camera's initial stack, its stack is empty) meets
  // the medium's back face: a true interface whose remove is refused, so stack and flags stay as they
  // were, and the hit is shaded as any other: an opacity-0 face passes, an opaque one is a bounce.
  Rig r(2, false, true);
  r.medium(0, 0, 0), r.medium(1, 1, 0);
  r.opacity(0, 0.0f);
  r.hit(0, 0, false), r.hit(1, 1, false);
  r.arrive(1, {{0, 0}});  // inside medium 0 only, leaving medium 1
  r.run();
  CHECK(r.next_stage[0] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
  CHECK(r.bounces[0].v[5] == 1u);
  CHECK(ar::medium_depth(r.flags[0]) == 0u);
  for (int k = 0; k < 4; ++k) CHECK(r.medium_stack[0].v[k] == 0u);
  CHECK(r.bounces[1].v[0] == 1u);
  CHECK(ar::medium_depth(r.flags[1]) == 1u);
  CHECK(r.medium_stack[1].v[0] == ar::stack_entry(1, 0));
  CHECK(ar::crossings(r.flags[0] | r.flags[1]) == 0u);
}

}  // TEST_SUITE
