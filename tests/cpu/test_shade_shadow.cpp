// SPDX-License-Identifier: Apache-2.0
// The shadow stage pair on hand-built items, shade_shadow_medium (the media pre-pass) then shade_shadow,
// as the scheduler launches them (0006 D8, D19; 0004 D4, D5): an item in no medium delivers its
// contribution bit for bit as before media; an item whose segment lies in a medium delivers
// contribution x T_ray x avg(r_l0 + r_u0) / avg(r_l0 rescale_l + r_u0 rescale_u) (pbrt-v4 SampleLd's
// weight from the tracked r_u, r_l), whose mean is Beer-Lambert per lane for a light-only item; and
// the NEE emit in shade_surface writes every media field of the item it emits, nothing without media;
// the scheduler launches the pre-pass before shade_shadow with the media feature only.
// The medium is homogeneous with chromatic RGB-sigmoid absorption (the program {reserved,
// VOLUME_COEFFS, END}); its sigma per lane is the volume entry's own value, so the truth is
// exp(-sigma d) at the item's lanes. With the majorant equal to sigma_t (the stage's), ratio
// tracking reaches the end with T_ray = T / T[h] and both factors the same, or stops at its first
// collision: for a delivered item the contribution is c T avg(r0) / avg(r0 T) exactly, and the mean
// over hero lanes (T[h] each) is c T per lane.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, D19; 0004 D1, D4, D5; 0003 d20, d23;
// pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::SampleLd (Apache-2.0).
#include <doctest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "../../include/astroray/astroray.h"
#include "common/stat.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/film/film_layout.h"
#include "host/scheduler.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_shadow.h"
#include "kernel/stages/shade_shadow_medium.h"
#include "kernel/stages/shade_surface.h"
#include "stub_stages.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Real;
using ar::Spec;

namespace {

namespace st = astroray::stat;

const float kNaN = std::nanf("");

// ---- the stage on hand-built items ----

// n items, item i in shadow slot i with target i, k samples per pixel (camera_key: pixel i / k,
// sample index i % k). Every item starts in no medium with contribution 1, the lanes 450-660 nm, a
// segment of length 1 m along +z (ray_d of length 2, tmax 0.5: the stage measures in metres), r_u0
// 0, r_l0 1, medium_rng at event 3's start and the hero lane i & 3.
struct ItemRig {
  uint32_t n;
  std::vector<uint32_t> queue, target, active, rng, slot_flags;
  std::vector<uint8_t> next, iflags;
  std::vector<ar::Real3> o, d;
  std::vector<Real> tmax;
  std::vector<Spec> contrib, lambda, r_u, r_l;
  std::vector<ar::U32x4> stack;
  std::vector<float> slot_L;
  std::vector<uint32_t> program{0u, uint32_t(ar::Op::VOLUME_COEFFS), uint32_t(ar::Op::END)};
  ar::MediumBlock block = ar::medium_block_vacuum();
  ar::MediumDesc desc{};
  ar::SceneData scene{};
  ar::SceneTables tables{};
  ar::FilmData film{};
  ar::ShadowItemSoA shadow{};
  ar::StepTable table{};
  ar::KernelGlobals kg{};
  ItemRig(uint32_t count, uint32_t k) : n(count) {
    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), target.push_back(i), rng.push_back(ar::shadow_rng_start(3u)), slot_flags.push_back(0u);
      next.push_back(uint8_t(ar::Stage::SHADE_SHADOW)), iflags.push_back(uint8_t(ar::item_hero_bits(i & 3u)));
      o.push_back(ar::Real3{0, 0, 0}), d.push_back(ar::Real3{0, 0, 2}), tmax.push_back(Real(0.5));
      contrib.push_back(ar::spec_fill(1)), lambda.push_back(Spec{{450, 520, 590, 660}});
      r_u.push_back(ar::spec_fill(0)), r_l.push_back(ar::spec_fill(1)), stack.push_back(ar::shadow_medium_field(0u));
    }
    for (uint32_t j = 0; j < (n + k - 1) / k; ++j) active.push_back(j);
    slot_L.assign(4ull * n, 0.0f);
    block.entry = 1u;
    scene.medium_descs = &desc, scene.medium_count = 1;
    scene.medium_blocks = &block, scene.medium_programs = program.data(), scene.medium_program_words = 3;
    shadow.next_stage = next.data(), shadow.ray_o = o.data(), shadow.ray_d = d.data(), shadow.tmax = tmax.data();
    shadow.contribution = contrib.data(), shadow.target = target.data(), shadow.item_flags = iflags.data();
    shadow.lambda_t = lambda.data(), shadow.r_u = r_u.data(), shadow.r_l_film_pos = r_l.data();
    shadow.medium_stack = stack.data(), shadow.medium_rng = rng.data();
    film.slot_capacity = n, film.slot_flags = slot_flags.data(), film.slot_L = slot_L.data();
    film.bounce_sum = film.bounce_max = film.bounce_paths = film.term_hist = -1;
    kg.scene = &scene, kg.shadow = &shadow, kg.shadow_queue = queue.data(), kg.tables = &tables, kg.film = &film;
    kg.active_pixels = active.data();
    table.stage[uint32_t(ar::Stage::SHADE_SHADOW)] = ar::StepEntry{0, n, 0};
    kg.step_table = &table;
    kg.session.samples_per_pass = k, kg.session.seed = 23, kg.session.sampler_mode = ar::SAMPLER_SOBOL;
    kg.session.features.media = true;
  }
  // Chromatic absorption scale * sigmoid(0.02 lambda - 11) per m (0.11 at 450 nm to 0.90 at 660 nm of
  // the scale), no scattering.
  void chromatic_medium(float scale) {
    block.sigma_a.kind = uint8_t(ar::SpectrumKind::RGB), block.sigma_a.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
    block.sigma_a.p[0] = 0.0f, block.sigma_a.p[1] = 0.02f, block.sigma_a.p[2] = -11.0f, block.sigma_a.p[3] = scale;
    block.lobe_count = 0;
    for (ar::U32x4& s : stack) s = ar::shadow_medium_field(1u);
  }
  // sigma_t of the medium at item i's lanes, from its volume entry.
  Spec sigma_t(uint32_t i) const {
    ar::Lambda lam;
    for (int j = 0; j < ar::kLanes; ++j) lam.nm[j] = lambda[i].v[j];
    ar::MediumSample ms = ar::medium_sample_zero();
    ar::VmRegs regs;
    REQUIRE(ar::medium_eval(ar::MediumTables{program.data(), &block, tables}, desc, o[i], lam, &regs, &ms) == ar::VmStatus::OK);
    return ms.sigma_a + ms.sigma_s;
  }
  // The scheduler's order: the media pre-pass (only with the media feature), then shade_shadow.
  void run() {
    if (kg.session.features.media) cpu_shade_shadow_medium(kg, 0, n);
    cpu_stage_shade_shadow(kg, ar::StepEntry{0, n, 0}, 0, n);
  }
  float lane(uint32_t i, int j) const { return slot_L[4ull * i + uint32_t(j)]; }
};

// ---- the NEE emit on a real scene ----

constexpr ar_key kMesh = 100;

// A quad of half-size 1 at z = 0 facing +z (instance 0, the default material); slots 0-3 hit it from
// above (front face), slots 4-7 from below (back face), at points of its first triangle, each with its
// own sample id, event index and hero lane. One point lamp, pmf 1 (delta: weight 1 / avg(r_u p_l)).
// The instance bears medium 0 when `medium` is set (a copied SceneData), and then the paths that hit
// it from below came from its interior: their stack holds the medium (id 1, priority 0, depth 1). The
// shadow pool's media fields are allocated and poisoned.
struct EmitRig {
  static constexpr uint32_t n = 8;
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
  ar::SceneData scene_copy{};
  std::vector<ar::DeviceInstanceRecord> instances;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<Real> hit_t, eta_scale, prev_dist;
  std::vector<Spec> lambda_t, beta, r_u, r_l;
  std::vector<uint32_t> sh_queue, sh_target, sh_rng;
  std::vector<uint8_t> sh_next, sh_flags;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<Real> sh_tmax;
  std::vector<Spec> sh_contrib, sh_lambda, sh_r_u, sh_r_l;
  std::vector<ar::U32x4> sh_stack, stack;
  std::vector<uint32_t> rng_path;
  std::vector<ar::LightRecord> lamps;
  std::vector<ar::AliasBin> bins{{1.0f, 0u}};
  std::vector<uint32_t> bin_light{0u};
  std::vector<float> pmf{1.0f};
  ar::LightSamplerView sampler{};

  EmitRig(const ar::LightRecord& lamp, bool media, bool medium) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
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
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = &kMesh;
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);
    FilmRequest req;
    req.max_width = n, req.max_height = 1, req.slot_capacity = n;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0 / set->lights->radiance_scale;
    const Spec poison = ar::spec_fill(kNaN);
    for (uint32_t i = 0; i < n; ++i) {
      const bool below = i >= 4;
      queue.push_back(i), active.push_back(i), sample_id.push_back(n - 1 - i);
      flags.push_back(ar::set_medium_depth(ar::set_hero_lane(0u, (i + 1u) & 3u), medium && below ? 1u : 0u));
      stack.push_back(ar::U32x4{{medium && below ? 1u << 8 : 0u, 0, 0, 0}}), rng_path.push_back(0u);
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u), event_index.push_back(uint16_t(1 + i % 4));
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{Real(0.1 + 0.15 * (i % 4)), Real(0.2 + 0.1 * (i % 4))});
      hit_t.push_back(Real(1));
      ray_o.push_back(ar::Real3{0, 0, below ? Real(-1) : Real(1)});
      ray_d.push_back(ar::Real3{Real(0.1), Real(-0.2), below ? Real(1) : Real(-1)});
      eta_scale.push_back(Real(1));
      prev_ng.push_back(0u), prev_ns.push_back(0u), prev_dist.push_back(Real(0));
      Spec lt, b, ru;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = Real(430 + 70 * l + int(i));
        b.v[l] = Real(0.5 + 0.125 * double(l));
        ru.v[l] = Real(1.0 + 0.25 * double(l) + 0.01 * double(i));
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(ru);
      sh_queue.push_back(n - 1 - i), sh_target.push_back(~0u);
      sh_next.push_back(uint8_t(ar::Stage::SHADOW_TERMINATED));
      sh_o.push_back(ar::Real3{0, 0, 0}), sh_d.push_back(ar::Real3{0, 0, 0}), sh_tmax.push_back(Real(-1));
      sh_contrib.push_back(ar::spec_fill(0));
      sh_flags.push_back(0xFFu), sh_rng.push_back(0xDEADBEEFu), sh_lambda.push_back(poison), sh_r_u.push_back(poison);
      sh_r_l.push_back(poison), sh_stack.push_back(ar::U32x4{{0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu}});
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data();
    path.prev_dist = prev_dist.data();
    if (media) path.medium_stack = stack.data(), path.medium_rng = rng_path.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();
    shadow.next_stage = sh_next.data(), shadow.ray_o = sh_o.data(), shadow.ray_d = sh_d.data();
    shadow.tmax = sh_tmax.data(), shadow.contribution = sh_contrib.data(), shadow.target = sh_target.data();
    shadow.item_flags = sh_flags.data(), shadow.lambda_t = sh_lambda.data(), shadow.r_u = sh_r_u.data();
    shadow.r_l_film_pos = sh_r_l.data(), shadow.medium_stack = sh_stack.data(), shadow.medium_rng = sh_rng.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 5;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path, kg.shadow = &shadow, kg.main_queue = queue.data(), kg.shadow_queue = sh_queue.data();
    kg.session.features.media = media;
    if (!media) {  // as the host allocates the pool: no media fields (and no environment world here)
      shadow.item_flags = nullptr, shadow.lambda_t = nullptr, shadow.r_u = nullptr, shadow.r_l_film_pos = nullptr;
      shadow.medium_stack = nullptr, shadow.medium_rng = nullptr;
    }
    scene_copy = *kg.scene;
    instances.assign(scene_copy.instances, scene_copy.instances + scene_copy.instance_count);
    if (medium) instances[0].medium = 1u;
    scene_copy.instances = instances.data();
    scene_copy.medium_count = medium ? 1u : 0u;
    kg.scene = &scene_copy;
    lamps = {lamp};
    sampler = ar::LightSamplerView{bins.data(), bin_light.data(), pmf.data(), 1u, 1u};
    kg.lights = lamps.data(), kg.light_sampler = &sampler;
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
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  // The hit context and record of pool slot s, as the stage builds them before it runs.
  void vertex(uint32_t s, ar::ShadeContext* c, ar::OpenPBRParams* rec) {
    REQUIRE(ar::shade_context_build(kg, s, c));
    const ar::SceneData& sd = *kg.scene;
    const ar::MaterialHeader& h = sd.material_headers[sd.material_header_count];  // the default material
    ar::resolve_constant_record(sd.material_constants[h.const_offset], *c, kg.tables->observer, sd.material_spectra, rec);
  }
};

bool bits_equal(const Spec& a, const Spec& b) { return std::memcmp(&a, &b, sizeof(Spec)) == 0; }

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)); }
ar::LightRecord point_lamp(ar::Real3 p) { return ar::light_make(ar::LightType::POINT, p, Real(0), Real(10), white()); }

// ---- the scheduler's launch of the media pre-pass ----

// What the counting pre-pass saw: its items, and those still unshaded (shade_shadow not yet run).
struct MediumSeen {
  std::atomic<uint32_t> items{0}, before{0};
};
MediumSeen& medium_seen() {
  static MediumSeen s;
  return s;
}
void counting_medium(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const ar::StepEntry& step = kg.step_table->stage[uint32_t(ar::Stage::SHADE_SHADOW)];
  for (uint32_t i = first; i < last; ++i) {
    ++medium_seen().items;
    if (ar::sh_next_stage(kg, kg.shadow_queue[step.start + i]) == uint8_t(ar::Stage::SHADE_SHADOW)) ++medium_seen().before;
  }
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_shadow/empty medium item delivers as before") {
  // Three items in no medium, their media fields other than the stack poisoned: with the media
  // feature the delivered slots are bit for bit those without it, and both are the contribution, as
  // the stage added it before media (sample_add into a zero slot).
  const Spec c[3] = {Spec{{Real(0.3125), Real(1.75), Real(0.0625), Real(2.5)}},
                     Spec{{Real(1e-3), Real(3.7e-2), Real(0.71), Real(5.3)}}, Spec{{Real(7), Real(0), Real(0.5), Real(1)}}};
  std::vector<float> slots[2];
  for (int media = 0; media < 2; ++media) {
    ItemRig r(3, 1);
    for (uint32_t i = 0; i < 3; ++i) {
      r.contrib[i] = c[i];
      r.r_u[i] = r.r_l[i] = r.lambda[i] = ar::spec_fill(kNaN);
      r.rng[i] = 0xDEADBEEFu;
    }
    if (media == 0) {
      r.kg.session.features.media = false;
      r.shadow.r_u = nullptr, r.shadow.medium_stack = nullptr, r.shadow.medium_rng = nullptr;
    }
    r.run();
    for (uint32_t i = 0; i < 3; ++i) CHECK(r.next[i] == uint8_t(ar::Stage::SHADOW_TERMINATED));
    if (media == 1) for (uint32_t i = 0; i < 3; ++i) CHECK(r.rng[i] == 0xDEADBEEFu);  // no tracking
    slots[media] = r.slot_L;
  }
  CHECK(std::memcmp(slots[0].data(), slots[1].data(), slots[0].size() * sizeof(float)) == 0);
  for (uint32_t i = 0; i < 3; ++i)
    for (int j = 0; j < ar::kLanes; ++j) CHECK(slots[1][4 * i + uint32_t(j)] == float(c[i].v[j]));
}

TEST_CASE("shade_shadow/medium item is attenuated") {
  // 2^14 light-only items (r_u0 = 0) over 1 m of the chromatic medium, contribution c per lane, hero
  // lanes cycling: per lane the mean delivered value is c exp(-sigma d) within 3 standard errors
  // (fixed seed), and every delivered item took one draw of its stream.
  ItemRig r(1u << 14, 64);
  r.chromatic_medium(1.5f);
  const Spec c{{Real(0.5), Real(1), Real(1.5), Real(2)}};
  for (Spec& x : r.contrib) x = c;
  for (Spec& x : r.r_l) x = ar::spec_fill(Real(0.7));
  r.run();
  const Spec sig = r.sigma_t(0);
  REQUIRE(double(sig.v[3]) > 4.0 * double(sig.v[0]));  // chromatic enough that a lane mix-up shows
  double sum[4] = {0, 0, 0, 0}, sum2[4] = {0, 0, 0, 0};
  uint32_t delivered = 0, bad_rng = 0, open = 0;
  for (uint32_t i = 0; i < r.n; ++i) {
    open += r.next[i] != uint8_t(ar::Stage::SHADOW_TERMINATED);
    const bool got = r.lane(i, 0) != 0.0f;
    delivered += got;
    bad_rng += r.rng[i] != ar::shadow_rng_start(3u) + 1u;
    for (int j = 0; j < 4; ++j) sum[j] += double(r.lane(i, j)), sum2[j] += double(r.lane(i, j)) * double(r.lane(i, j));
  }
  CHECK(open == 0u);
  CHECK(bad_rng == 0u);
  CHECK(delivered > r.n / 4);
  CHECK(delivered < r.n);
  for (int j = 0; j < 4; ++j) {
    const double mean = sum[j] / r.n, se = std::sqrt((sum2[j] / r.n - mean * mean) / (r.n - 1));
    const double truth = double(c.v[j]) * std::exp(-double(sig.v[j]));
    std::printf("shade_shadow medium lane %d: mean %.6f truth %.6f se %.2e z %+.2f\n", j, mean, truth, se, (mean - truth) / se);
    INFO("lane " << j);
    CHECK(std::fabs(mean - truth) < 3.0 * se);
  }
}

TEST_CASE("shade_shadow/medium item carries the tracked weight") {
  // Items with both strategies' rescaled pdfs (r_u0, r_l0 differing per lane): a delivered item
  // reached the end with factors T / T[h], so it holds c T avg(r_l0 + r_u0) / avg((r_l0 + r_u0) T)
  // per lane, recomputed here in double; the hero lane cancels. An item over 0 m delivers c.
  ItemRig r(64, 1);
  r.chromatic_medium(0.4f);
  const Spec c{{Real(1.25), Real(0.5), Real(2), Real(0.75)}};
  const Spec ru0{{Real(0.2), Real(0.9), Real(0.4), Real(1.6)}}, rl0{{Real(1.1), Real(0.3), Real(0.8), Real(0.6)}};
  for (uint32_t i = 0; i < r.n; ++i) r.contrib[i] = c, r.r_u[i] = ru0, r.r_l[i] = rl0;
  r.tmax[63] = Real(0);
  r.run();
  const Spec sig = r.sigma_t(0);
  double tm[4], num = 0.0, den = 0.0;
  for (int j = 0; j < 4; ++j) {
    tm[j] = std::exp(-double(sig.v[j]));
    num += double(rl0.v[j]) + double(ru0.v[j]);
    den += (double(rl0.v[j]) + double(ru0.v[j])) * tm[j];
  }
  uint32_t delivered = 0;
  for (uint32_t i = 0; i < 63; ++i) {
    if (r.lane(i, 0) == 0.0f) continue;
    ++delivered;
    for (int j = 0; j < 4; ++j) {
      const double want = double(c.v[j]) * tm[j] * num / den;
      INFO("item " << i << " lane " << j);
      CHECK(std::fabs(double(r.lane(i, j)) - want) <= 2e-5 * want);
    }
  }
  CHECK(delivered > 32u);
  for (int j = 0; j < 4; ++j) CHECK(r.lane(63, j) == float(c.v[j]));
}

TEST_CASE("shade_shadow/medium pre-pass runs only with media") {
  // The scheduler with the stub stages (stub_stages.h: every surface hit emits a shadow item). Without
  // the media feature no SHADE_SHADOW_MEDIUM kernel is registered, so a launch would throw: the pass
  // completing shows none. With it, the pre-pass runs over every item of every SHADE_SHADOW segment,
  // each still unshaded, and the deliveries are the same.
  std::vector<uint32_t> delivered[2];
  for (int media = 0; media < 2; ++media) {
    ContextState ctx;
    Ledger ledger;
    ThreadPool pool(ctx, 2);
    CpuDevice dev(ledger, pool);
    stub::Results res(3000, 0);
    dev.register_scheduler_kernels();
    dev.register_kernel(KernelId::GENERATE_CAMERA, &stub::generate_camera);
    dev.register_stage(ar::Stage::INTERSECT_CLOSEST, &stub::intersect_closest);
    dev.register_stage(ar::Stage::INTERSECT_SHADOW, &stub::intersect_shadow);
    dev.register_stage(ar::Stage::SHADE_BACKGROUND, &stub::shade_background);
    dev.register_stage(ar::Stage::SHADE_LIGHT, &stub::shade_light);
    dev.register_stage(ar::Stage::SHADE_SURFACE, &stub::shade_surface);
    dev.register_stage(ar::Stage::SHADE_SHADOW, &stub::shade_shadow);
    if (media) dev.register_kernel(KernelId::SHADE_SHADOW_MEDIUM, &counting_medium);
    ar::SceneData scene{};
    SchedulerConfig cfg;
    cfg.globals.film = reinterpret_cast<ar::FilmData*>(&res);
    cfg.globals.scene = &scene;
    cfg.features.media = media == 1;
    Scheduler sched(dev, cfg);
    medium_seen().items = 0, medium_seen().before = 0;
    REQUIRE(sched.run_pass(PassWork{3000, 0}) == PassStatus::COMPLETE);
    const uint64_t shaded = sched.counters().items[uint32_t(ar::Stage::SHADE_SHADOW)];
    INFO("media " << media);
    CHECK(shaded > 1000u);
    CHECK(uint64_t(medium_seen().items.load()) == (media ? shaded : 0u));
    CHECK(uint64_t(medium_seen().before.load()) == (media ? shaded : 0u));
    delivered[media] = res.shadow_delivered;
  }
  CHECK(delivered[0] == delivered[1]);
}

TEST_CASE("shade_shadow/emit defines medium fields") {
  // With the media feature, every item the emit writes has all its media fields defined: medium_rng
  // at its event's block start, the path's lanes and hero lane, r_u0 = 0 and r_l0 = r_p (a point
  // lamp of pmf 1: delta, p_l = 1), and the stack naming the instance's medium exactly for items of the
  // paths whose stack holds it, on their side of the surface (lamp below, hit from below). Without the feature
  // the poison stays.
  struct Run {
    Real lamp_z;
    bool media, medium;
  };
  for (const Run run : {Run{2, true, true}, Run{-2, true, true}, Run{-2, true, false}, Run{-2, false, true}}) {
    EmitRig rig(point_lamp(ar::Real3{Real(0.3), Real(0.2), run.lamp_z}), run.media, run.medium);
    rig.run();
    const bool below = run.lamp_z < 0;
    uint32_t items = 0;
    for (uint32_t s = 0; s < EmitRig::n; ++s) {
      const uint32_t it = rig.sh_queue[s];
      const bool emitted = rig.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW);
      INFO("lamp z " << run.lamp_z << " media " << run.media << " medium " << run.medium << " slot " << s);
      CHECK(emitted == (below == (s >= 4)));  // only the side the lamp lights
      if (!emitted) continue;
      ++items;
      if (!run.media) {
        CHECK(rig.sh_flags[it] == 0xFFu);
        CHECK(rig.sh_rng[it] == 0xDEADBEEFu);
        CHECK(rig.sh_stack[it].v[0] == 0xDEADBEEFu);
        CHECK(std::isnan(double(rig.sh_r_u[it].v[0])));
        CHECK(std::isnan(double(rig.sh_r_l[it].v[0])));
        CHECK(std::isnan(double(rig.sh_lambda[it].v[0])));
        continue;
      }
      const uint32_t m = run.medium && below ? 1u : 0u;
      CHECK(rig.sh_stack[it].v[0] == m);
      for (int k = 1; k < 4; ++k) CHECK(rig.sh_stack[it].v[k] == 0u);
      CHECK(rig.sh_rng[it] == ar::shadow_rng_start(1u + s % 4u));  // the event index the path arrived with
      CHECK(rig.sh_flags[it] == ar::item_hero_bits(ar::hero_lane(rig.flags[s])));
      CHECK(bits_equal(rig.sh_lambda[it], rig.lambda_t[s]));
      CHECK(bits_equal(rig.sh_r_l[it], rig.r_u[s]));
      CHECK(bits_equal(rig.sh_r_u[it], ar::spec_fill(0)));
    }
    CHECK(items == 4u);
  }
}

TEST_CASE("shade_shadow/area light item takes the stack's medium") {
  // A non-delta light's item lies in the medium too: the BSDF half of its MIS pair is delta-tracked
  // (route_hit sends every segment of a non-empty stack to shade_volume), so both halves are attenuated.
  // A rectangle lamp below the quad, the instance bearing a medium: the back-face hits came from its
  // interior (their stack holds it) and emit items on that side, whose medium field names it. The other
  // fields are defined as SampleLd forms them: r_l0 = r_p p_l (pmf 1) and r_u0 = r_p p_bsdf for the
  // light sample the stage drew, recomputed here as in nee/rectangle lamp carries the balance weight,
  // and avg(r_l0 + r_u0) times the emit's weight mis_nee_scale is 1.
  const ar::LightRecord rect = ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{Real(0.2), 0, Real(-1.5)},
                                                   ar::Real3{0, 1, 0}, ar::Real3{1, 0, 0}, Real(0.8), Real(1.0), Real(20), white());
  EmitRig rig(rect, true, true);
  std::vector<ar::ShadeContext> c(EmitRig::n);
  std::vector<ar::OpenPBRParams> rec(EmitRig::n);
  for (uint32_t s = 4; s < EmitRig::n; ++s) rig.vertex(s, &c[s], &rec[s]);
  const std::vector<Spec> r_p = rig.r_u;
  rig.run();
  const auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-5 * std::fabs(b); };
  uint32_t items = 0;
  for (uint32_t s = 4; s < EmitRig::n; ++s) {
    const uint32_t it = rig.sh_queue[s];
    INFO("slot " << s);
    REQUIRE(rig.sh_next[it] == uint8_t(ar::Stage::INTERSECT_SHADOW));
    ++items;
    CHECK(rig.sh_stack[it].v[0] == 1u);  // 1 + the medium index: the item layout of ratio_track.h
    for (int k = 1; k < 4; ++k) CHECK(rig.sh_stack[it].v[k] == 0u);
    const uint32_t e = 1u + s % 4u;  // the event index the path arrived with
    const ar::Real2 u{ar::camera_path_draw(rig.kg, rig.sample_id[s], ar::event_dim(e, ar::EV_LIGHT_U)),
                      ar::camera_path_draw(rig.kg, rig.sample_id[s], ar::event_dim(e, ar::EV_LIGHT_V))};
    const ar::LightSampleContext lc = ar::light_context_point(ar::offset_ray_origin(c[s].P, c[s].Ng, c[s].wo), c[s].Ng, c[s].N);
    const ar::LightLiSample ls = ar::light_sample_li(rect, lc, u, c[s].lambda_t, rig.kg.tables->observer);
    REQUIRE(ls.pdf > 0.0f);
    REQUIRE(!ls.delta);
    const ar::LobeEval ev = ar::stack_eval(*rig.kg.e_diel, *rig.kg.e_spec, rec[s], ar::frame_from_normal(c[s].N), c[s].wo, ls.wi,
                                           ar::TransportMode::Radiance);
    REQUIRE(ev.pdf > 0.0f);
    for (int k = 0; k < ar::kLanes; ++k) {
      CHECK(near(double(rig.sh_r_l[it].v[k]), double(r_p[s].v[k]) * double(ls.pdf)));
      CHECK(near(double(rig.sh_r_u[it].v[k]), double(r_p[s].v[k]) * double(ev.pdf)));
    }
    const double w = double(ar::mis_nee_scale(r_p[s], ls.pdf, ev.pdf, false));
    CHECK(near(double(ar::avg(rig.sh_r_l[it] + rig.sh_r_u[it])) * w, 1.0));
  }
  CHECK(items == 4u);
}

TEST_CASE("shade_shadow/collision limit counts volume limit") {
  // The pre-pass's per-item function with a limit of 0 blocks: item 1, in the chromatic medium, stops at
  // the limit before any draw, ends blocked (contribution 0, SHADOW_TERMINATED, medium field cleared) and
  // counts VOLUME_LIMIT exactly once in the termination histogram, at its target's pixel; item 0 is
  // untouched.
  ItemRig r(4, 1);
  r.chromatic_medium(1.5f);
  const uint32_t pixels = uint32_t(r.active.size());
  std::vector<uint32_t> counts(size_t(ar::kTermCount) * pixels, 0u);
  r.film.count = counts.data(), r.film.plane_stride = pixels, r.film.term_hist = 0;
  ar::shadow_medium_item(r.kg, 1u, 0u);
  const uint32_t pixel = ar::film_slot_pixel(r.kg, r.target[1]);
  uint32_t wrong = 0;
  for (uint32_t t = 0; t < ar::kTermCount; ++t)
    for (uint32_t p = 0; p < pixels; ++p)
      wrong += counts[size_t(t) * pixels + p] != (t == uint32_t(ar::Term::VOLUME_LIMIT) && p == pixel ? 1u : 0u);
  CHECK(wrong == 0u);
  CHECK(counts[size_t(ar::Term::VOLUME_LIMIT) * pixels + pixel] == 1u);
  CHECK(r.next[1] == uint8_t(ar::Stage::SHADOW_TERMINATED));
  CHECK(bits_equal(r.contrib[1], ar::spec_fill(0)));
  CHECK(r.stack[1].v[0] == 0u);
  CHECK(r.rng[1] == ar::shadow_rng_start(3u));  // no block drawn
  CHECK(r.next[0] == uint8_t(ar::Stage::SHADE_SHADOW));
  CHECK(bits_equal(r.contrib[0], ar::spec_fill(1)));
}
TEST_CASE("shade_shadow/capped equals uncapped") {
  // 0006 D8 requeue in the media pre-pass: 256 items in the chromatic medium under a majorant 16 sigma_t
  // (the gate hook's value, RULINGS 111), so estimates take null collisions, with r_u0 and r_l0 differing
  // per lane so the final weight reads all three tracked factors. Delivered at the default cap in one
  // step, and at caps 1 and 7 in as many steps as it takes, each step the pre-pass and shade_shadow
  // over the items still in SHADE_SHADOW, the tracker state in the side buffer between steps. Every
  // delivery, end and counter is bit-identical; a capped item is not delivered in its step.
  static constexpr uint32_t kItems = 256;  // static: the lambdas below use it without capturing it
  struct Run {
    std::vector<float> L;
    std::vector<uint8_t> next;
    std::vector<uint32_t> rng;
    uint32_t steps = 0, skipped_ok = 1;
  };
  const auto run = [](uint32_t cap) {
    auto r = std::make_unique<ItemRig>(kItems, 4);
    r->chromatic_medium(3.0f);
    r->kg.session.medium_majorant = 16u;
    r->kg.session.medium_launch_cap = cap;
    std::vector<ar::ShadowTrack> track(kItems);
    r->kg.shadow_track = track.data();
    for (uint32_t i = 0; i < kItems; ++i) {
      r->r_u[i] = Spec{{Real(0.25), Real(0.5), Real(1), Real(2)}} * Real(1 + (i & 7u));
      r->r_l[i] = Spec{{Real(1.5), Real(1), Real(0.75), Real(0.5)}};
    }
    Run out;
    std::vector<uint32_t> live(kItems);
    for (uint32_t i = 0; i < kItems; ++i) live[i] = i;
    while (!live.empty()) {
      ++out.steps;
      const uint32_t n = uint32_t(live.size());
      r->kg.shadow_queue = live.data();
      r->table.stage[uint32_t(ar::Stage::SHADE_SHADOW)] = ar::StepEntry{0, n, 0};
      cpu_shade_shadow_medium(r->kg, 0, n);
      cpu_stage_shade_shadow(r->kg, ar::StepEntry{0, n, 0}, 0, n);
      std::vector<uint32_t> again;
      for (uint32_t i : live) {
        if (r->next[i] != uint8_t(ar::Stage::SHADE_SHADOW)) continue;
        again.push_back(i);
        // Capped this step: still in its medium and nothing delivered yet.
        out.skipped_ok &= r->stack[i].v[0] != 0u && r->lane(i, 0) == 0.0f && r->lane(i, 3) == 0.0f;
      }
      live = again;
    }
    out.L = r->slot_L, out.next = r->next, out.rng = r->rng;
    return out;
  };
  const Run ref = run(ar::kMediumLaunchCap);
  CHECK(ref.steps == 1u);
  uint32_t delivered = 0;
  for (uint32_t i = 0; i < kItems; ++i) delivered += ref.L[4ull * i] > 0.0f;
  CHECK(delivered > kItems / 8);  // the estimates are not all zero, so equal bits mean something
  for (const uint32_t cap : {1u, 7u}) {
    const Run c = run(cap);
    INFO("cap " << cap);
    std::printf("shade_shadow capped: cap %u took %u steps\n", cap, c.steps);
    CHECK(c.steps > 2u);
    CHECK(c.skipped_ok == 1u);
    CHECK(c.next == ref.next);
    CHECK(c.rng == ref.rng);
    CHECK(std::memcmp(c.L.data(), ref.L.data(), c.L.size() * sizeof(float)) == 0);
  }
}

}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("shade_shadow/transmittance per lane") {
  // The CPU stage on 2^20 light-only items over 1 m of the chromatic medium, Sobol keys of 2^14
  // pixels x 64 samples, hero lanes cycling: per lane the mean delivered value against
  // exp(-sigma d), one gate member per lane. The CUDA twin is volume_agree/shadow transmittance cuda.
  const st::Run run = st::begin("shade_shadow/transmittance per lane", st::gate_family());
  ItemRig r(1u << 20, 64);
  r.chromatic_medium(1.5f);
  r.kg.session.seed = uint32_t(run.seed);
  r.run();
  const Spec sig = r.sigma_t(0);
  std::vector<double> samples[4];
  for (uint32_t i = 0; i < r.n; ++i)
    for (int j = 0; j < 4; ++j) samples[j].push_back(double(r.lane(i, j)));
  for (int j = 0; j < 4; ++j) {
    const double truth = std::exp(-double(sig.v[j]));
    const st::EnergyResult e = st::energy_test(run, samples[j], truth);
    std::printf("shade_shadow transmittance lane %d: mean %.6f truth %.6f se %.2e z %+.2f\n", j, e.mean, truth, e.se, e.z);
    CHECK_MESSAGE(e.pass, e.message);
  }
}

}  // TEST_SUITE
