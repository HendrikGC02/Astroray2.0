// SPDX-License-Identifier: Apache-2.0
// shade_surface, part 2 (task 073): continuation. Hand-built hits on a real scene's tables, with
// the material's constant block replaced by the test's own (a Lambertian base, or a pure specular
// layer). A Lambertian sample gives beta * f |cos| / pdf, r_u unchanged and r_l = r_u / pdf
// (pbrt-v4 VolPathIntegrator::Li; 0004 D1), the counters, event index, stored vertex and next ray;
// Russian roulette follows 0004 D2 on a fixed uniform and preserves the mean; min_bounces moves
// its start; the total limit ends MAX_BOUNCE; a reflection below Ng ends ZERO_THROUGHPUT; the
// octahedral normals round-trip; the continuation ray types are the ABI's visibility bits.
// Ref: docs/decisions/0004-integrator.md D1, D2, D3, D14; 0003 d19, d21-d23; 0002 D16; Cigolle et
// al. 2014 (octahedral encoding).
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
constexpr uint32_t kMaxBounces = 8;

// One quad of half-size 1 at z = 0 facing +z with one material (an Emission graph whose constant
// block the rig then replaces), instanced once.
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
  std::vector<ar::DeviceMeshView> meshes;
  std::vector<float> nrm;
  ar::PathStateSoA path{};
  uint32_t n;
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l;

  explicit Rig(uint32_t count) : n(count) {
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
      std::vector<ar_key> geom{kMesh}, object{500}, keys{kMat};
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

    FilmRequest req;
    req.max_width = n;
    req.max_height = 1;
    req.slot_capacity = n;
    req.statistics = true;
    req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0;

    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(ar::LAST_BOUNCE_SPECULAR, i & 3u));  // stale: must be cleared
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
      prev_ng.push_back(0u), prev_ns.push_back(0u), prev_dist.push_back(ar::Real(7));
      ar::Spec lt, b, ru;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = ar::Real(430 + 70 * l);
        b.v[l] = ar::Real(0.5 + 0.125 * double(l));
        ru.v[l] = ar::Real(1.0 + 0.25 * double(l));  // r_u not 1, so an update of it would show
      }
      lambda_t.push_back(lt), beta.push_back(b), r_u.push_back(ru), r_l.push_back(ru);
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
    in.uniforms.seed = 11;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = kMaxBounces;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
    // The material's constant block, replaced by the test's (host memory: the CPU stage reads it).
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
  ar::ConstantBlock& block() { return blocks[sd.material_headers[sd.material_slots[0]].const_offset]; }
  static ar::SpectrumRef grey(float v) {
    ar::SpectrumRef r{};
    r.kind = uint8_t(ar::SpectrumKind::CONSTANT);
    r.p[0] = v;
    return r;
  }
  // A Lambertian base of albedo rho, no specular layer, no emission.
  void lambert(float rho) {
    ar::ConstantBlock& b = block();
    b.base_weight = 1.0f, b.base_color = grey(rho), b.base_diffuse_roughness = 0.0f, b.base_metalness = 0.0f;
    b.specular_weight = 0.0f, b.emission_luminance = 0.0f;
  }
  // Only the specular layer (base weight 0): every sample is GGX.
  void specular() {
    ar::ConstantBlock& b = block();
    b.base_weight = 0.0f, b.specular_weight = 1.0f, b.specular_color = grey(1.0f), b.specular_roughness = 0.4f;
    b.emission_luminance = 0.0f;
  }
  // Tilts every corner normal of the quad (host copy of the mesh view).
  void tilt_normals(float x, float z) {
    meshes.assign(sd.meshes, sd.meshes + sd.mesh_count);
    nrm.clear();
    for (int i = 0; i < 4; ++i) nrm.insert(nrm.end(), {x, 0.0f, z});
    meshes[0].corner_normals = uint64_t(uintptr_t(nrm.data()));
    sd.meshes = meshes.data();
  }
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  // The record and frame the stage samples with, for slot s, before it runs.
  void vertex(uint32_t s, ar::ShadeContext* c, ar::OpenPBRParams* rec) {
    REQUIRE(ar::shade_context_build(kg, s, c));
    ar::resolve_constant_record(block(), *c, kg.tables->observer, rec);
  }
  uint32_t hist(ar::Term t) const {
    const ar::FilmData& f = film->data();
    uint32_t total = 0;
    for (uint32_t p = 0; p < n; ++p) total += f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + p];
    return total;
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_surface_bsdf/lambert beta") {
  Rig rig(1);
  rig.lambert(0.5f);
  ar::ShadeContext c{};
  ar::OpenPBRParams rec;
  rig.vertex(0, &c, &rec);
  const ar::Spec beta0 = rig.beta[0], ru0 = rig.r_u[0];
  rig.run();
  REQUIRE(rig.term[0] == uint8_t(ar::Term::ACTIVE));
  CHECK(rig.next_stage[0] == uint8_t(ar::Stage::INTERSECT_CLOSEST));
  const ar::Real3 wi = rig.ray_d[0];
  CHECK(std::fabs(double(ar::length(wi)) - 1.0) < 1e-6);
  // The expected update from the lobe stack's own eval and pdf at the sampled direction:
  // beta * f |cos theta_i| / pdf with cos against the shading normal, r_u unchanged, r_l = r_u / pdf.
  const ar::Frame frame = ar::frame_from_normal(c.N);
  const ar::LobeEval e = ar::stack_eval(*rig.kg.e_diel, rec, frame, c.wo, wi, ar::TransportMode::Radiance);
  REQUIRE(e.pdf > 0.0f);
  const double cos_i = std::fabs(double(ar::dot(wi, c.N)));
  for (int l = 0; l < ar::kLanes; ++l) {
    const double want = double(beta0.v[l]) * double(e.f.v[l]) * cos_i / double(e.pdf);
    CHECK(std::fabs(double(rig.beta[0].v[l]) - want) <= 1e-5 * want);
    // Base roughness 0 is Lambert: f cos / pdf is the albedo itself.
    CHECK(std::fabs(double(rig.beta[0].v[l]) - 0.5 * double(beta0.v[l])) <= 1e-4 * double(beta0.v[l]));
    CHECK(rig.r_u[0].v[l] == ru0.v[l]);
    CHECK(std::fabs(double(rig.r_l[0].v[l]) - double(ru0.v[l]) / double(e.pdf)) <= 1e-5 * double(rig.r_l[0].v[l]));
  }
  // Counters, event index, flags, the stored vertex and the next ray.
  CHECK(rig.bounces[0].v[0] == 1u);
  CHECK(rig.bounces[0].v[1] == 1u);
  CHECK(rig.bounces[0].v[2] == 0u);
  CHECK(rig.event_index[0] == 1u);
  CHECK((rig.flags[0] & ar::LAST_BOUNCE_SPECULAR) == 0u);
  CHECK(rig.ray_type[0] == ar::kRayTypeDiffuse);
  CHECK(rig.prev_ng[0] == ar::oct_encode(c.Ng));
  CHECK(rig.prev_ns[0] == ar::oct_encode(c.N));
  CHECK(rig.prev_dist[0] == 0.0f);
  const ar::Real3 o = ar::offset_ray_origin(c.P, c.Ng, wi);
  CHECK(rig.ray_o[0].x == o.x);
  CHECK(rig.ray_o[0].y == o.y);
  CHECK(rig.ray_o[0].z == o.z);
  CHECK(rig.ray_o[0].z > 0.0f);  // above the quad, on the side the ray leaves to
  CHECK(wi.z > 0.0f);
}

TEST_CASE("shade_surface_bsdf/a glossy sample counts as glossy") {
  Rig rig(64);
  rig.specular();
  rig.run();
  uint32_t active = 0;
  for (uint32_t s = 0; s < rig.n; ++s) {
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE)) continue;
    ++active;
    CHECK(rig.ray_type[s] == ar::kRayTypeGlossy);
    CHECK(rig.bounces[s].v[0] == 1u);
    CHECK(rig.bounces[s].v[1] == 0u);
    CHECK(rig.bounces[s].v[2] == 1u);
  }
  CHECK(active > rig.n / 2);
}

TEST_CASE("shade_surface_bsdf/continuation ray types are the visibility bits") {
  CHECK(AR_VISIBILITY_DIFFUSE == ar::kRayTypeDiffuse);
  CHECK(AR_VISIBILITY_GLOSSY == ar::kRayTypeGlossy);
}

TEST_CASE("shade_surface_bsdf/roulette on a fixed uniform") {
  // beta (0.1, 0.2, 0.3, 0.4), eta_scale 2, r_u average 2: max_lane(beta eta / avg r_u) = 0.4,
  // q = 0.6 (0004 D2).
  const ar::Spec b{{0.1f, 0.2f, 0.3f, 0.4f}}, ru{{1.0f, 1.5f, 2.5f, 3.0f}};
  CHECK(ar::roulette_q(b, 2.0f, ru, 1, 0) == 0.0f);  // depth 1: never
  CHECK(std::fabs(ar::roulette_q(b, 2.0f, ru, 2, 0) - 0.6f) < 1e-6f);
  CHECK(ar::roulette_q(b, 2.0f, ru, 4, 5) == 0.0f);  // below min_bounces
  CHECK(std::fabs(ar::roulette_q(b, 2.0f, ru, 5, 5) - 0.6f) < 1e-6f);
  const ar::Spec big{{0.1f, 0.2f, 0.3f, 1.2f}};
  CHECK(ar::roulette_q(big, 1.0f, ar::spec_fill(1.0f), 9, 0) == 0.0f);  // max lane above 1: no roulette
  ar::Spec s = b;
  CHECK(!ar::roulette(&s, 2.0f, ru, 2, 0, 0.59f));  // u < q: ends
  s = b;
  REQUIRE(ar::roulette(&s, 2.0f, ru, 2, 0, 0.61f));  // survives, weighted 1 / (1 - q)
  for (int l = 0; l < ar::kLanes; ++l) CHECK(std::fabs(s.v[l] - b.v[l] / 0.4f) <= 1e-6f * s.v[l]);
}

TEST_CASE("shade_surface_bsdf/roulette preserves the mean") {
  // Over stratified uniforms u_i = (i + 1/2) / N the survivors' weights average to beta within the
  // stratification error 1 / (N (1 - q)), for every q.
  constexpr uint32_t kN = 1u << 16;
  const ar::Spec ru{{1.0f, 1.0f, 1.0f, 1.0f}};
  for (float m : {0.9f, 0.5f, 0.1f, 0.01f}) {
    const ar::Spec b{{m, m * 0.5f, m * 0.25f, m * 0.125f}};
    double sum = 0.0;
    for (uint32_t i = 0; i < kN; ++i) {
      ar::Spec s = b;
      if (ar::roulette(&s, 1.0f, ru, 3, 0, float((double(i) + 0.5) / kN))) sum += double(s.v[0]);
    }
    CHECK(std::fabs(sum / kN - double(m)) <= double(m) / (kN * double(m)) + 1e-6 * double(m));
  }
}

TEST_CASE("shade_surface_bsdf/min_bounces moves the roulette start") {
  // A path whose throughput makes q = 1 - 1e-6 at the vertex of depth kMaxBounces (arriving with
  // kMaxBounces - 1 bounces). With min_bounces = max_bounces + 1 no vertex that can continue
  // reaches the start, so nothing ends RUSSIAN_ROULETTE; with min_bounces = max_bounces only that
  // last continuation is rouletted (0004 D2 with pbrt's depth), and the one before it is not.
  for (uint32_t min_b : {kMaxBounces + 1u, kMaxBounces}) {
    for (uint16_t arrive : {uint16_t(kMaxBounces - 1u), uint16_t(kMaxBounces - 2u)}) {
      Rig rig(256);
      rig.lambert(0.5f);
      rig.kg.session.min_bounces = min_b;
      for (uint32_t s = 0; s < rig.n; ++s) {
        rig.bounces[s].v[0] = arrive;
        rig.r_u[s] = ar::spec_fill(1.0f);
        rig.beta[s] = ar::spec_fill(2e-6f);  // times rho 0.5: max lane 1e-6
      }
      rig.run();
      const bool rouletted = min_b == kMaxBounces && arrive == kMaxBounces - 1u;
      CHECK(rig.hist(ar::Term::RUSSIAN_ROULETTE) == (rouletted ? rig.n : 0u));
      uint32_t active = 0;
      for (uint32_t s = 0; s < rig.n; ++s) active += rig.term[s] == uint8_t(ar::Term::ACTIVE);
      CHECK(active == (rouletted ? 0u : rig.n));
    }
  }
}

TEST_CASE("shade_surface_bsdf/the total limit ends after emission") {
  Rig rig(4);
  rig.lambert(0.5f);
  rig.block().emission_color = Rig::grey(1.0f);
  rig.block().emission_luminance = 1.0f;
  for (uint32_t s = 0; s < rig.n; ++s) rig.bounces[s].v[0] = uint16_t(kMaxBounces);
  rig.run();
  CHECK(rig.hist(ar::Term::MAX_BOUNCE) == rig.n);
  for (uint32_t s = 0; s < rig.n; ++s) {
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    CHECK(rig.event_index[s] == 0u);
    CHECK(rig.film->data().slot_L[4u * s] > 0.0f);  // the emission at the vertex was added
  }
}

TEST_CASE("shade_surface_bsdf/reflections below the geometric surface end") {
  // Corner normals tilted 60 degrees from Ng: a cosine lobe about N puts many directions under the
  // quad, which must end ZERO_THROUGHPUT; every continuing ray leaves above it (0005 item 8).
  Rig rig(2048);
  rig.lambert(0.5f);
  rig.tilt_normals(0.866f, 0.5f);
  for (uint32_t s = 0; s < rig.n; ++s) rig.ray_d[s] = ar::Real3{ar::Real(-0.5), 0, ar::Real(-1)};
  rig.run();
  uint32_t zero = 0, active = 0;
  for (uint32_t s = 0; s < rig.n; ++s) {
    zero += rig.term[s] == uint8_t(ar::Term::ZERO_THROUGHPUT);
    if (rig.term[s] != uint8_t(ar::Term::ACTIVE)) continue;
    ++active;
    CHECK(rig.ray_d[s].z > 0.0f);
  }
  CHECK(zero > rig.n / 20);
  CHECK(active > rig.n / 2);
  CHECK(zero + active == rig.n);
}

TEST_CASE("shade_surface_bsdf/octahedral normals round-trip") {
  // Cigolle et al. 2014: 2 x 16-bit octahedral codes are accurate to about 1e-4 rad. The angle is
  // measured in double (atan2 of |v x w| and v . w): acos of a float dot cannot resolve it.
  const auto angle = [](ar::Real3 a, ar::Real3 b) {
    const double ax = a.x, ay = a.y, az = a.z, bx = b.x, by = b.y, bz = b.z;
    const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
  };
  double worst = 0.0;
  for (uint32_t i = 0; i < 20000; ++i) {
    const double z = 1.0 - 2.0 * (double(i) + 0.5) / 20000.0, phi = 2.399963229728653 * double(i);
    const double r = std::sqrt(1.0 - z * z);
    const ar::Real3 v{float(r * std::cos(phi)), float(r * std::sin(phi)), float(z)};
    const ar::Real3 w = ar::oct_decode(ar::oct_encode(v));
    worst = std::fmax(worst, angle(v, w));
  }
  for (const ar::Real3 axis : {ar::Real3{0, 0, 1}, ar::Real3{0, 0, -1}, ar::Real3{1, 0, 0}, ar::Real3{0, -1, 0}}) {
    const ar::Real3 w = ar::oct_decode(ar::oct_encode(axis));
    worst = std::fmax(worst, angle(axis, w));
  }
  CHECK(worst < 1e-4);
}

}  // TEST_SUITE
