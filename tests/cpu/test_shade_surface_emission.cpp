// SPDX-License-Identifier: Apache-2.0
// shade_surface, part 1 (task 071): hand-built hits on a real scene's tables. Emission from a
// constant block and from a program is added exactly once, times beta and the radiance scale s;
// the primary-hit AOVs are written once, IndexMA is the material's pass index, and a mesh without
// materials shades through the host's default material; FRONT_FACE survives a mirroring instance
// transform and a back-face hit flips N toward the viewer; a VM error or a non-finite
// contribution ends the path INVALID. A gate case renders a material-less mesh through a CPU
// session with no invalid sample.
// Ref: docs/decisions/0004-integrator.md D5, D17, D28; 0003 d10, d21, d22; 0005 items 33, 35, 38;
// 0008 d6, d10; 0007 appendix (count 0 means the default material).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/resolve.h"
#include "host/film/film_layout.h"
#include "host/film/film_view.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_surface.h"
#include "kernel_cpu/stage_cpu.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

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

constexpr ar_key kMesh = 100, kA = 1, kB = 2;
constexpr uint32_t kPassA = 3, kPassB = 7;

struct SceneRig {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  SceneRig() {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
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
  void material(Edit& e, ar_key key, const Graph& g, uint32_t pass_index) {
    const ar_graph_desc d = g.desc();
    REQUIRE(scene.materials->set_graph(e, key + 1000, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = key + 1000;
    m.pass_index = pass_index;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, key, &m) == AR_OK);
  }
  // A square of half-size h at z (facing +z), one UV layer with uv = ((x/h + 1) / 2, (y/h + 1) / 2).
  void quad(Edit& e, float h, float z) {
    const std::vector<float> pos{-h, -h, z, h, -h, z, h, h, z, -h, h, z}, uv{0, 0, 1, 0, 1, 1, 0, 1};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    ar_mesh_desc d = {};
    d.key = kMesh, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1, d.uv_layer_count = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    REQUIRE(b.uv_count == uv.size());
    std::memcpy(*b.uv, uv.data(), uv.size() * 4);
    REQUIRE(e.item_commit(kMesh) == AR_OK);
  }
  // Instances of the quad: x translation, x scale (-1 mirrors) and material slice each.
  struct Inst {
    float tx, sx;
    std::vector<ar_key> pool;
  };
  void instances(Edit& e, const std::vector<Inst>& list) {
    const uint32_t n = uint32_t(list.size());
    std::vector<ar_key> geom(n, kMesh), object(n), pool;
    std::vector<ar_xform> xf(n);
    std::vector<uint32_t> zeros(n, 0), vis(n, AR_VISIBILITY_CAMERA), moff(n), mcount(n), pass(n);
    for (uint32_t i = 0; i < n; ++i) {
      object[i] = 500 + i;
      xf[i].m[0] = list[i].sx, xf[i].m[5] = xf[i].m[10] = 1.0f, xf[i].m[3] = list[i].tx;
      moff[i] = uint32_t(pool.size()), mcount[i] = uint32_t(list[i].pool.size());
      pass[i] = 40 + i;
      pool.insert(pool.end(), list[i].pool.begin(), list[i].pool.end());
    }
    std::vector<ar_rgba> colors(n, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(n, 0);
    ar_instances_desc d = {};
    d.instance_count = n, d.motion_steps = 1, d.transform_count = n, d.material_key_count = uint32_t(pool.size());
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.crypto_objects = d.crypto_assets = zeros.data();
    d.pass_indices = pass.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
};

// Instances 0-3: A, B (program emission), no materials (the default), A mirrored in x.
const std::vector<SceneRig::Inst> kInstances = {{0.0f, 1.0f, {kA}}, {10.0f, 1.0f, {kB}}, {20.0f, 1.0f, {}}, {30.0f, -1.0f, {kA}}};

constexpr uint32_t kSlots = 5;  // slot i hits instance i; slot 4 hits instance 0 from behind
constexpr double kInvS = 0.25;  // s = 4, so a missing or doubled s shows

struct HitRig {
  SceneRig sr;
  std::shared_ptr<const SceneTableSet> set;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t;
  std::vector<ar::Spec> lambda_t, beta;
  // r_u = 1: emission at the first vertex is divided by avg(r_u) (074), so the values stay beta Le s.
  std::vector<ar::Spec> r_u = std::vector<ar::Spec>(kSlots, ar::spec_fill(1.0f));
  ar::StepEntry step{0, kSlots, 0};

  HitRig() {
    Graph ga, gb;
    ga.emit({1.0f, 0.5f, 0.25f}, 2.0f);  // a lone constant emission: the constant block
    gb.emit({0.2f, 0.4f, 0.6f}, 1.5f);   // two emissions: the surface program's EMIT_LE sum
    gb.emit({1.0f, 1.0f, 1.0f}, 0.5f);
    sr.commit([&](Edit& e) {
      sr.material(e, kA, ga, kPassA);
      sr.material(e, kB, gb, kPassB);
      sr.quad(e, 1.0f, 0.0f);
    });
    sr.commit([&](Edit& e) { sr.instances(e, kInstances); });
    set = acquire_scene_tables(sr.scene);
    REQUIRE(set);

    FilmRequest req;
    req.max_width = kSlots;
    req.max_height = 1;
    req.slot_capacity = kSlots;
    req.statistics = true;
    req.termination_stats = true;
    req.primary_aov = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = kInvS;

    for (uint32_t i = 0; i < kSlots; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i);
      flags.push_back(ar::set_hero_lane(0u, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE));
      ray_type.push_back(1u);
      bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(i < 4 ? i : 0u), hit_prim.push_back(0u);
      hit_uv.push_back(ar::Real2{ar::Real(0.2), ar::Real(0.3)});
      hit_t.push_back(ar::Real(1));
      ray_d.push_back(ar::Real3{0, 0, i < 4 ? ar::Real(-2) : ar::Real(2)});  // not unit: the context normalises
      ar::Spec lt, b;
      for (int l = 0; l < ar::kLanes; ++l) {
        lt.v[l] = ar::Real(420 + 60 * l + 7 * int(i));
        b.v[l] = ar::Real(0.5 + 0.25 * double(l) + 0.125 * double(i));
      }
      lambda_t.push_back(lt), beta.push_back(b);
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.ray_d = ray_d.data(), path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data();
    path.hit_uv = hit_uv.data();

    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = kSlots, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path;
    kg.main_queue = queue.data();
  }
  void run() { cpu_stage_shade_surface(kg, step, 0, kSlots); }
  ar::Spec slot_value(uint32_t s) const {
    ar::Spec v;
    for (int l = 0; l < ar::kLanes; ++l) v.v[l] = film->data().slot_L[4u * s + uint32_t(l)];
    return v;
  }
  float add_plane(int32_t first, uint32_t c, uint32_t pixel) const {
    const ar::FilmData& f = film->data();
    return f.add[uint64_t(uint32_t(first) + c) * f.plane_stride + pixel];
  }
  uint32_t hist(ar::Term t, uint32_t pixel) const {
    const ar::FilmData& f = film->data();
    return f.count[uint64_t(uint32_t(f.term_hist) + uint32_t(t)) * f.plane_stride + pixel];
  }
  // What the stage must add for slot s, from the same pieces in the same order: the block's
  // emission, then the surface entry's EMIT_LE sum, times s, times beta.
  ar::Spec expected(uint32_t s) const {
    // The wavelengths from the path's own array, so a context that mangled them would show.
    ar::Lambda lam{};
    for (int l = 0; l < ar::kLanes; ++l) lam.nm[l] = lambda_t[s].v[l];
    ar::ShadeContext c{};
    REQUIRE(ar::shade_context_build(kg, s, &c));
    c.lambda_t = lam;
    const ar::SceneData& sd = *kg.scene;
    const uint32_t mat = ar::hit_material_id(kg, hit_inst[s], 0);
    const ar::MaterialHeader& h = sd.material_headers[mat];
    const ar::ConstantBlock& b = sd.material_constants[h.const_offset];
    ar::Spec le = ar::resolve(b.emission_color, lam, kg.tables->observer) * ar::Real(b.emission_luminance);
    if ((h.flags & ar::CONSTANT_SURFACE) == 0u) {
      ar::VmRegs regs;
      ar::OpenPBRParams rec;
      const uint32_t* prog = sd.material_programs + h.program_offset;
      REQUIRE(ar::run_entry(prog, prog[0], &regs, c, *kg.tables, &rec, &le) == ar::VmStatus::OK);
    }
    return le * ar::radiance_scale(kg) * beta[s];
  }
};

// The rig's scene with mesh 0's arrays and instance 0's transform replaced by the test's own
// (host memory: the CPU stage reads it directly). Counts stay the quad's: 4 vertices and corners,
// 2 triangles, one UV layer.
struct SceneOverride {
  ar::SceneData sd{};
  ar::DeviceMeshView mv{};
  std::vector<ar::DeviceInstanceRecord> inst;
  std::vector<float> pos, nrm;
  std::vector<uint32_t> tri;  // vertex = corner index, 2 x 3
  void install(HitRig& rig, const float* xf12) {
    sd = *rig.kg.scene;
    mv = sd.meshes[0];
    if (!pos.empty()) mv.positions = uint64_t(uintptr_t(pos.data()));
    if (!nrm.empty()) mv.corner_normals = uint64_t(uintptr_t(nrm.data()));
    if (!tri.empty()) mv.tri_vertex = mv.tri_corners = uint64_t(uintptr_t(tri.data()));
    inst.assign(sd.instances, sd.instances + sd.instance_count);
    if (xf12) std::memcpy(inst[0].object_to_world, xf12, sizeof inst[0].object_to_world);
    sd.meshes = &mv;
    sd.instances = inst.data();
    rig.kg.scene = &sd;
  }
};

bool same(const ar::Spec& a, const ar::Spec& b) {
  for (int l = 0; l < ar::kLanes; ++l)
    if (a.v[l] != b.v[l]) return false;
  return true;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("shade_surface/emission added once") {
  HitRig rig;
  const auto mats = rig.sr.scene.materials->arrays();
  // A's emission folded into its block; B's lives in its program (both emissions, in order).
  const uint32_t ha = rig.kg.scene->material_slots[0], hb = rig.kg.scene->material_slots[1];
  CHECK((rig.kg.scene->material_headers[ha].flags & ar::CONSTANT_SURFACE) != 0u);
  CHECK(rig.kg.scene->material_constants[ha].emission_luminance == 2.0f);
  CHECK((rig.kg.scene->material_headers[hb].flags & ar::CONSTANT_SURFACE) == 0u);
  CHECK(rig.kg.scene->material_constants[hb].emission_luminance == 0.0f);
  CHECK(ar::radiance_scale(rig.kg) == ar::Real(4));
  const ar::Spec want_a = rig.expected(0), want_b = rig.expected(1);
  rig.run();
  CHECK(same(rig.slot_value(0), want_a));
  CHECK(same(rig.slot_value(1), want_b));
  CHECK(same(rig.slot_value(4), rig.expected(4)));  // two-sided: the back face emits as well
  for (uint32_t s : {0u, 1u})
    for (int l = 0; l < ar::kLanes; ++l) CHECK(rig.slot_value(s).v[l] > 0.0f);
  for (uint32_t s = 0; s < kSlots; ++s) {
    CHECK(rig.term[s] == uint8_t(ar::Term::MAX_BOUNCE));
    CHECK(rig.next_stage[s] == uint8_t(ar::Stage::TERMINATED));
    CHECK(rig.hist(ar::Term::MAX_BOUNCE, s) == 1u);
    CHECK(rig.film->data().slot_flags[s] == rig.flags[s]);
  }
  // The default material emits nothing and does not fail.
  for (int l = 0; l < ar::kLanes; ++l) CHECK(rig.slot_value(2).v[l] == 0.0f);
  CHECK(mats->keys.size() == 2u);
  CHECK(rig.kg.scene->material_header_count == 2u);
}

TEST_CASE("shade_surface/AOVs written once with the material pass index") {
  HitRig rig;
  rig.run();
  const ar::FilmData& f = rig.film->data();
  const float want_index[kSlots] = {float(kPassA), float(kPassB), 0.0f, float(kPassA), float(kPassA)};
  const float tx[kSlots] = {0, 10, 20, 30, 0}, sx[kSlots] = {1, 1, 1, -1, 1};
  std::vector<float> first;
  for (uint32_t p = 0; p < kSlots; ++p) {
    CHECK((rig.flags[p] & ar::AOV_WRITTEN) != 0u);
    CHECK(rig.add_plane(f.aov_material, 0, p) == want_index[p]);
    // UV is affine in object position on the quad, so P follows from the UV AOV whichever triangle
    // the quad's prim 0 is.
    const float u = rig.add_plane(f.aov_uv, 0, p), v = rig.add_plane(f.aov_uv, 1, p);
    CHECK(u > 0.0f);
    CHECK(u < 1.0f);
    CHECK(std::fabs(rig.add_plane(f.aov_position, 0, p) - (tx[p] + sx[p] * (2.0f * u - 1.0f))) < 1e-5f);
    CHECK(std::fabs(rig.add_plane(f.aov_position, 1, p) - (2.0f * v - 1.0f)) < 1e-5f);
    CHECK(rig.add_plane(f.aov_position, 2, p) == 0.0f);
    CHECK(rig.add_plane(f.aov_normal, 0, p) == 0.0f);
    CHECK(rig.add_plane(f.aov_normal, 1, p) == 0.0f);
    // Facing normals: +z toward the cameras above, -z for the hit from below.
    CHECK(rig.add_plane(f.aov_normal, 2, p) == (p == 4 ? -1.0f : 1.0f));
    for (int32_t pl : {f.aov_position, f.aov_normal})
      for (uint32_t c = 0; c < 3; ++c) first.push_back(rig.add_plane(pl, c, p));
  }
  // A second visit of the same paths (as after a pass-through) writes nothing.
  for (uint32_t s = 0; s < kSlots; ++s) rig.next_stage[s] = uint8_t(ar::Stage::SHADE_SURFACE);
  rig.run();
  std::vector<float> again;
  for (uint32_t p = 0; p < kSlots; ++p)
    for (int32_t pl : {f.aov_position, f.aov_normal})
      for (uint32_t c = 0; c < 3; ++c) again.push_back(rig.add_plane(pl, c, p));
  CHECK(again == first);
}

TEST_CASE("shade_surface/front face survives a mirroring instance") {
  HitRig rig;
  rig.run();
  CHECK((rig.flags[0] & ar::FRONT_FACE) != 0u);
  CHECK((rig.flags[3] & ar::FRONT_FACE) != 0u);  // scale x = -1: still the front, seen from +z
  CHECK((rig.flags[4] & ar::FRONT_FACE) == 0u);  // the same instance from below
  ar::ShadeContext c{};
  REQUIRE(ar::shade_context_build(rig.kg, 3, &c));
  CHECK(c.front_face == 1u);
  CHECK(c.Ng.z == ar::Real(1));
  CHECK(c.instance_id == 3u);
  CHECK(c.object_id == 43u);  // the instance's pass index
  REQUIRE(ar::shade_context_build(rig.kg, 4, &c));
  CHECK(c.front_face == 0u);
  CHECK(c.Ng.z == ar::Real(-1));
  CHECK(c.N.z == ar::Real(-1));
  CHECK(c.wo.z == ar::Real(-1));
}

TEST_CASE("shade_surface/barycentric convention") {
  // hit_uv (u, v) weights corners 1 and 2 of the triangle (geom/hit.h): the UV AOV of prim 0 at
  // (0.2, 0.3) is (1 - u - v) uv[c0] + u uv[c1] + v uv[c2], with the corners the triangulation
  // stored for prim 0.
  HitRig rig;
  rig.run();
  const ar::DeviceMeshView& mv = rig.kg.scene->meshes[0];
  const uint32_t* corners = reinterpret_cast<const uint32_t*>(uintptr_t(mv.tri_corners));
  const float* uv = reinterpret_cast<const float*>(uintptr_t(mv.uv));
  const double u = 0.2, v = 0.3, w = 1.0 - u - v;
  for (uint32_t k = 0; k < 2; ++k) {
    const double want = w * uv[2 * corners[0] + k] + u * uv[2 * corners[1] + k] + v * uv[2 * corners[2] + k];
    CHECK(std::fabs(double(rig.add_plane(rig.film->data().aov_uv, k, 0)) - want) < 1e-6);
  }
  // Corners 1 and 2 differ in UV, so swapping u and v would show.
  CHECK((uv[2 * corners[1]] != uv[2 * corners[2]] || uv[2 * corners[1] + 1] != uv[2 * corners[2] + 1]));
}

TEST_CASE("shade_surface/zero corner normals fall back to Ng") {
  HitRig rig;
  SceneOverride o;
  o.nrm.assign(12, 0.0f);
  o.install(rig, nullptr);
  ar::ShadeContext c{};
  REQUIRE(ar::shade_context_build(rig.kg, 0, &c));
  CHECK(c.N.x == c.Ng.x);
  CHECK(c.N.y == c.Ng.y);
  CHECK(c.N.z == c.Ng.z);
  CHECK(c.Ng.z == ar::Real(1));
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) CHECK(rig.term[s] == uint8_t(ar::Term::MAX_BOUNCE));
  CHECK(rig.add_plane(rig.film->data().aov_normal, 2, 0) == 1.0f);
}

TEST_CASE("shade_surface/a degenerate triangle ends invalid") {
  HitRig rig;
  SceneOverride o;
  o.pos.assign(12, 0.5f);  // every vertex at one point: no geometric normal
  o.install(rig, nullptr);
  ar::ShadeContext c{};
  CHECK(!ar::shade_context_build(rig.kg, 0, &c));
  rig.run();
  for (uint32_t s = 0; s < kSlots; ++s) {
    CHECK(rig.term[s] == uint8_t(ar::Term::INVALID));
    CHECK(!ar::film_sample_valid(rig.slot_value(s)));
  }
}

TEST_CASE("shade_surface/normals under a sheared non-uniform instance") {
  // M = rows (2, 0.5, 0), (0, 1, 0), (0, 0, 0.5) on a tilted triangle with tilted corner normals.
  // The expected normals come from M^-T built here by Gauss-Jordan inversion in double.
  HitRig rig;
  SceneOverride o;
  o.pos = {0, 0, 0, 1, 0, 0.5f, 0, 1, 0.3f, 1, 1, 0.8f};
  o.nrm = {0.1f, 0.2f, 1.0f, -0.2f, 0.1f, 1.0f, 0.3f, -0.1f, 0.9f, 0.0f, 0.0f, 1.0f};
  o.tri = {0, 1, 2, 0, 2, 3};
  const float xf[12] = {2, 0.5f, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 0};
  o.install(rig, xf);
  double a[3][6] = {{2, 0.5, 0, 1, 0, 0}, {0, 1, 0, 0, 1, 0}, {0, 0, 0.5, 0, 0, 1}};
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    for (int r = col + 1; r < 3; ++r)
      if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
    for (int k = 0; k < 6; ++k) std::swap(a[col][k], a[piv][k]);
    const double d = a[col][col];
    for (int k = 0; k < 6; ++k) a[col][k] /= d;
    for (int r = 0; r < 3; ++r)
      if (r != col) {
        const double f = a[r][col];
        for (int k = 0; k < 6; ++k) a[r][k] -= f * a[col][k];
      }
  }
  // inv[r][c] = a[r][3 + c], so (M^-T n)_r = sum_c inv[c][r] n_c.
  const auto inv_t = [&](const double n[3], double out[3]) {
    double l = 0;
    for (int r = 0; r < 3; ++r) {
      out[r] = a[0][3 + r] * n[0] + a[1][3 + r] * n[1] + a[2][3 + r] * n[2];
      l += out[r] * out[r];
    }
    for (int r = 0; r < 3; ++r) out[r] /= std::sqrt(l);
  };
  const double u = 0.2, v = 0.3, w = 1.0 - u - v;
  const double e1[3] = {1, 0, 0.5}, e2[3] = {0, 1, 0.3};
  const double ng_obj[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  double n_obj[3];
  for (int k = 0; k < 3; ++k) n_obj[k] = w * o.nrm[k] + u * o.nrm[3 + k] + v * o.nrm[6 + k];
  double ng_want[3], n_want[3];
  inv_t(ng_obj, ng_want);
  inv_t(n_obj, n_want);
  CHECK(ng_want[2] > 0.0);  // seen from +z, the front: no flip in the context
  ar::ShadeContext c{};
  REQUIRE(ar::shade_context_build(rig.kg, 0, &c));
  CHECK(c.front_face == 1u);
  const double ng_got[3] = {c.Ng.x, c.Ng.y, c.Ng.z}, n_got[3] = {c.N.x, c.N.y, c.N.z};
  for (int k = 0; k < 3; ++k) {
    CHECK(std::fabs(ng_got[k] - ng_want[k]) < 1e-6);
    CHECK(std::fabs(n_got[k] - n_want[k]) < 1e-6);
  }
}

TEST_CASE("shade_surface/vm error and non-finite contributions end invalid") {
  HitRig rig;
  // B's surface program with its first instruction replaced by an opcode outside the table.
  ar::SceneData sd = *rig.kg.scene;
  std::vector<uint32_t> prog(sd.material_programs, sd.material_programs + rig.kg.material_program_words);
  const uint32_t hb = sd.material_slots[1];
  const uint32_t base = sd.material_headers[hb].program_offset;
  prog[base + prog[base]] = (prog[base + prog[base]] & ~0xffu) | 0xffu;
  sd.material_programs = prog.data();
  rig.kg.scene = &sd;
  rig.beta[0].v[2] = std::numeric_limits<float>::quiet_NaN();
  rig.run();
  for (uint32_t s : {0u, 1u}) {
    CHECK(rig.term[s] == uint8_t(ar::Term::INVALID));
    CHECK(rig.hist(ar::Term::INVALID, s) == 1u);
    CHECK(!ar::film_sample_valid(rig.slot_value(s)));
  }
  for (uint32_t s : {2u, 3u, 4u}) CHECK(rig.term[s] == uint8_t(ar::Term::MAX_BOUNCE));
}

TEST_CASE("shade_surface/film writes go to the sample's slot") {
  // A refill that handed out the pool slots in another order: pool slot i carries sample p[i].
  // Emission, AOVs, flags and path-end statistics all land on the sample's slot and pixel (0004 D5).
  HitRig rig;
  const uint32_t p[kSlots] = {3, 4, 0, 1, 2};
  ar::Spec want[kSlots];
  for (uint32_t i = 0; i < kSlots; ++i) rig.sample_id[i] = p[i];
  for (uint32_t i = 0; i < kSlots; ++i) want[i] = rig.expected(i);
  rig.run();
  const ar::FilmData& f = rig.film->data();
  const float index[kSlots] = {float(kPassA), float(kPassB), 0.0f, float(kPassA), float(kPassA)};
  for (uint32_t i = 0; i < kSlots; ++i) {
    CHECK(same(rig.slot_value(p[i]), want[i]));
    CHECK(f.slot_flags[p[i]] == rig.flags[i]);
    CHECK(rig.hist(ar::Term::MAX_BOUNCE, p[i]) == 1u);
    CHECK(rig.add_plane(f.aov_material, 0, p[i]) == index[i]);
  }
}

TEST_CASE("shade_surface/the cpu session device runs the stage") {
  ContextState ctx;
  std::shared_ptr<Device> dev = make_cpu_session_device(ctx, 1);
  CHECK(static_cast<const CpuDevice&>(*dev).stage_fn(ar::Stage::SHADE_SURFACE) == &cpu_stage_shade_surface);
}

}  // TEST_SUITE

namespace {

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

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_surface/a mesh without materials renders through the default material") {
  constexpr uint32_t kW = 6, kH = 4, kSpp = 4;
  SceneRig sr;
  Graph world;
  world.emit({1.0f, 1.0f, 1.0f}, 1.0f);  // a lit world: a ray that missed the quad would not be black
  sr.commit([&](Edit& e) {
    const ar_graph_desc dw = world.desc();
    ar_world_desc w = {};
    w.surface_graph = 40;
    REQUIRE(sr.scene.materials->set_graph(e, 40, &dw) == AR_OK);
    REQUIRE(sr.scene.materials->set_world(e, &w) == AR_OK);
    sr.quad(e, 20.0f, -1.0f);  // fills the camera's view at z = -1
    ar_settings s{};
    s.samples = s.viewport_samples = kSpp;
    s.max_bounces = 0;  // the path ends at the quad: this case tests emission, not reflection
    s.pixel_filter = AR_FILTER_BOX;
    s.pixel_filter_width = 1.0f;
    s.film_exposure = 1.0f;
    s.seed = 5;
    REQUIRE(e.set_settings(&s) == AR_OK);
  });
  sr.commit([&](Edit& e) { sr.instances(e, {{0.0f, 1.0f, {}}}); });

  std::shared_ptr<Device> dev = make_cpu_session_device(sr.ctx, 1);
  SessionOptions o;
  o.max_width = kW;
  o.max_height = kH;
  Session session(sr.scene, *dev, o);
  session.set_view(view_of(kW, kH));
  session.start();
  REQUIRE(session.wait(60000));
  REQUIRE(session.progress().state == AR_STATE_CONVERGED);
  std::vector<float> mean;
  std::vector<uint32_t> n, invalid;
  session.read_film([&](const FilmReadContext& c) {
    FilmView view(c.device, c.film);
    for (const PassDesc& p : list_passes(c.film.layout())) {
      if (p.name != "Combined") continue;
      mean.resize(pass_float_count(p, AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, p, AR_QUANTITY_MEAN, uint32_t(mean.size()), mean.data());
    }
    n = view.sample_counts();
    invalid = view.invalid_counts();
  });
  REQUIRE(mean.size() == size_t(kW) * kH * 3);
  // Every sample hit the quad, shaded through the default (no emission) and ended valid.
  for (uint32_t i = 0; i < kW * kH; ++i) {
    CHECK(n[i] == kSpp);
    CHECK(invalid[i] == 0u);
  }
  for (float m : mean) CHECK(m == 0.0f);
}

}  // TEST_SUITE
