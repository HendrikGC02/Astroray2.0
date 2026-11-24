// SPDX-License-Identifier: Apache-2.0
// shade_surface on the GPU against the CPU float stage (0009 d17 step iii, the per-stage fixture
// of 0002 D4): 10^5 synthetic surface hits on a hand-built scene of 8 instances (two of them
// mirroring, one without materials, so the default material is hit) of a random triangle soup,
// with materials compiled by the host compiler: constant-block emission, program emission (two
// EMIT_LEs), emission driven by UV set 0 and by P (the context reaches the VM), the default
// material, a Mix Shader of two BSDF children with a per-point factor, and one corrupt program
// (the VM-error path). Two samples per pixel share the path-end
// statistics and AOV planes; throughputs are 1% non-finite. Since 073 the paths continue: the
// materials' BSDF slots are the OpenPBR defaults of their constant blocks (a GGX layer over EON),
// arrival bounce counts 0-5 under a total limit of 8, so the lobe sample, Russian roulette and the
// next ray all run. Integer fields (term, next_stage, path flags, slot flags, every COUNT plane,
// the material-id AOV, bounce counters, event index, ray type) must be bit-equal; the slot
// radiance agrees to 0002 D12's evaluation threshold (FMA-off GPU build vs CPU float), beta and
// r_l to its throughput threshold and the next direction to its direction threshold; the
// position, normal and UV AOVs and the next ray origin within 1e-5 relative (0009 d17 step iv's
// geometry tolerance).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0004 D5, D28; 0008 d10.
// The production module is launched, so what runs is the shipped code.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/material/compiler.h"
#include "host/thread_pool.h"
#include "stages/shade_surface.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths
constexpr uint32_t kTris = 64, kInstances = 8, kSlotsPerInstance = 3;

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
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  void emit_from(int colour, float strength) {
    const int s = add(ar::Op::CONST_F32, {strength}), e = add(ar::Op::EMIT_LE, {});
    link(colour, e, 0), link(s, e, 1);
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

struct Tables {
  ObserverTables obs;
  ar::Rgb2SpecTable jh = {};
  Tables() {
    DataBlob b;
    REQUIRE(find_data("spectral/observer.bin", &b));
    const std::string err = parse_observer(b.data, b.size, &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    REQUIRE(find_data("spectral/rec709.jh", &b));
    const char* e = ar::jh_view(b.data, b.size, &jh);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
  }
};

// The material arrays as the host store lays them out: the scene's materials, then the default
// one past them (scene_view.h).
struct Materials {
  std::vector<ar::MaterialHeader> headers;
  std::vector<uint32_t> programs, pass_index;
  std::vector<ar::ConstantBlock> constants;
  uint32_t count = 0;  // material_header_count: the default is at this index
  void add(const MaterialOutput& m, uint32_t pass) {
    ar::MaterialHeader h = {};
    h.program_offset = uint32_t(programs.size()), h.const_offset = uint32_t(constants.size()), h.flags = m.flags;
    headers.push_back(h);
    programs.insert(programs.end(), m.compiled.program.begin(), m.compiled.program.end());
    constants.push_back(m.compiled.block);
    pass_index.push_back(pass);
  }
};

Materials build_materials(const Tables& t) {
  Materials ms;
  const auto compile = [&](const Graph& g) {
    const MaterialOutput m = compile_material(g.desc(), t.jh, nullptr, nullptr);
    REQUIRE(!m.magenta);
    return m;
  };
  Graph block, program, uv, p;
  block.emit_from(block.add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), 2.0f);
  program.emit_from(program.add(ar::Op::CONST_F32X3, {0.2f, 0.4f, 0.6f}), 1.5f);
  program.emit_from(program.add(ar::Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), 0.5f);
  uv.emit_from(uv.add(ar::Op::LOAD_CTX_UV, {}, {0}), 3.0f);
  p.emit_from(p.add(ar::Op::LOAD_CTX_P, {}), 0.75f);
  const MaterialOutput mb = compile(block), mp = compile(program), mu = compile(uv), mx = compile(p);
  REQUIRE((mb.flags & ar::CONSTANT_SURFACE) != 0u);
  REQUIRE((mp.flags & ar::CONSTANT_SURFACE) == 0u);
  REQUIRE((mu.flags & ar::CONSTANT_SURFACE) == 0u);
  ms.add(mb, 3), ms.add(mp, 7), ms.add(mu, 11), ms.add(mx, 13);
  // A Mix Shader (0005 item 38): factor UV.x per point, child 0 a base coloured by UV set 0 with no
  // specular layer, child 1 a rougher GGX layer over a constant colour. The selection rescales the
  // lobe-choice uniform (the entry's selection uniform) on both targets.
  Graph mix;
  const auto store = [&mix](int src, ar::RecordSlot s, int32_t child) {
    mix.link(src, mix.add(ar::Op::STORE_SLOT, {}, {int32_t(s), child}), 0);
  };
  const int muv = mix.add(ar::Op::LOAD_CTX_UV, {}, {0});
  const int fac = mix.add(ar::Op::CHANNEL_F32X3, {}, {0});
  mix.link(muv, fac, 0);
  store(muv, ar::RecordSlot::base_color, 0);
  store(mix.add(ar::Op::CONST_F32, {0.0f}), ar::RecordSlot::specular_weight, 0);
  store(mix.add(ar::Op::CONST_F32X3, {0.9f, 0.3f, 0.1f}), ar::RecordSlot::base_color, 1);
  store(mix.add(ar::Op::CONST_F32, {0.6f}), ar::RecordSlot::specular_roughness, 1);
  mix.link(fac, mix.add(ar::Op::BRANCH_MIX, {}, {0, 0, 1}), 0);
  const MaterialOutput mm = compile(mix);
  REQUIRE(mm.compiled.children.size() == 2u);
  ms.add(mm, 19);
  // A corrupt program: its first instruction's opcode is outside the table.
  MaterialOutput bad = mp;
  bad.compiled.program[bad.compiled.program[0]] |= 0xffu;
  ms.add(bad, 17);
  ms.count = uint32_t(ms.headers.size());
  const MaterialOutput dflt = compile_material(ar_graph_desc{}, t.jh, nullptr, nullptr);
  REQUIRE((dflt.flags & ar::CONSTANT_SURFACE) != 0u);
  ms.add(dflt, 0);
  return ms;
}

struct Snapshot {
  std::vector<float> slot_L;
  std::vector<ar::Spec> beta, r_l;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<ar::U16x6> bounces;
  std::vector<uint16_t> event_index;
  std::vector<uint8_t> ray_type;
  std::vector<uint32_t> slot_flags, flags;
  std::vector<uint8_t> next_stage, term;
  std::vector<uint32_t> counts;  // every COUNT plane
  std::vector<float> adds;       // every ADD plane
  int32_t aov_position = 0, aov_normal = 0, aov_uv = 0, aov_material = 0, bounce_sum = 0;
  uint32_t plane_stride = 0;
};

float rnd(uint32_t i, uint32_t d) { return float(agree::unit(i, d)); }

Snapshot run(Device& dev, ModuleDevice* gpu, const Tables& tb, const Materials& ms) {
  const Owner owner{1, 1};
  std::vector<DeviceBuffer> bufs;
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    bufs.push_back(b);
    return b.ptr;
  };
  FilmRequest req;
  req.max_width = kFilmW;
  req.max_height = kFilmH;
  req.slot_capacity = kN;
  req.statistics = true;
  req.termination_stats = true;
  req.primary_aov = true;
  Film film(dev, FilmLayout::build(req), owner);
  film.data().inv_radiance_scale = 0.5;  // s = 2
  void* film_p = put(&film.data(), sizeof(ar::FilmData));

  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  void* queue_p = put(identity.data(), kN * 4);
  void* list_p = put(list.data(), list.size() * 4);
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_SURFACE)] = step;
  void* table_p = put(&table, sizeof table);

  // The mesh: kTris independent triangles, corner normals not unit length, two UV layers.
  std::vector<float> pos(9 * kTris), nrm(9 * kTris), uv(2 * 2 * 3 * kTris);
  std::vector<uint32_t> tri_v(3 * kTris), tri_c(3 * kTris);
  std::vector<int32_t> tri_m(kTris);
  for (uint32_t t = 0; t < kTris; ++t) {
    for (uint32_t k = 0; k < 9; ++k) {
      pos[9 * t + k] = 2.0f * rnd(t, k) - 1.0f;
      nrm[9 * t + k] = (k % 3 == 2 ? 1.0f : 0.0f) + 0.6f * (rnd(t, 9 + k) - 0.5f);
    }
    for (uint32_t k = 0; k < 3; ++k) tri_v[3 * t + k] = tri_c[3 * t + k] = 3 * t + k;
    tri_m[t] = int32_t(t % kSlotsPerInstance);
  }
  for (size_t k = 0; k < uv.size(); ++k) uv[k] = rnd(uint32_t(k), 20);
  ar::DeviceMeshView mv{};
  mv.positions = uint64_t(uintptr_t(put(pos.data(), pos.size() * 4)));
  mv.tri_vertex = uint64_t(uintptr_t(put(tri_v.data(), tri_v.size() * 4)));
  mv.tri_corners = uint64_t(uintptr_t(put(tri_c.data(), tri_c.size() * 4)));
  mv.corner_normals = uint64_t(uintptr_t(put(nrm.data(), nrm.size() * 4)));
  mv.uv = uint64_t(uintptr_t(put(uv.data(), uv.size() * 4)));
  mv.tri_material = uint64_t(uintptr_t(put(tri_m.data(), tri_m.size() * 4)));
  mv.vertex_count = 3 * kTris, mv.triangle_count = kTris, mv.corner_count = 3 * kTris, mv.uv_layer_count = 2;

  // Instances: random rotation-scale-shear, two with negative determinant; instance 5 has no
  // materials. Each slice draws 3 of the 5 compiled materials.
  std::vector<ar::DeviceInstanceRecord> inst(kInstances);
  std::vector<uint32_t> pool;
  for (uint32_t j = 0; j < kInstances; ++j) {
    ar::DeviceInstanceRecord& r = inst[j];
    r = {};
    r.pass_index = 100 + j;
    for (uint32_t k = 0; k < 12; ++k) r.object_to_world[k] = (k % 5 == 0 ? 1.0f : 0.0f) + 0.8f * (rnd(1000 + j, k) - 0.5f);
    if (j == 2 || j == 6) r.object_to_world[0] = -r.object_to_world[0], r.object_to_world[4] = -r.object_to_world[4],
                          r.object_to_world[8] = -r.object_to_world[8];
    r.light_index = ar::kNoLightIndex;
    r.material_offset = uint32_t(pool.size());
    r.material_count = j == 5 ? 0u : kSlotsPerInstance;
    if (j != 5)
      for (uint32_t k = 0; k < kSlotsPerInstance; ++k) pool.push_back((j + 2 * k) % ms.count);
  }
  ar::SceneData sd{};
  sd.instances = static_cast<const ar::DeviceInstanceRecord*>(put(inst.data(), inst.size() * sizeof(inst[0])));
  sd.instance_count = kInstances;
  sd.meshes = static_cast<const ar::DeviceMeshView*>(put(&mv, sizeof mv));
  sd.mesh_count = 1;
  sd.material_slots = static_cast<const uint32_t*>(put(pool.data(), pool.size() * 4));
  sd.material_headers = static_cast<const ar::MaterialHeader*>(put(ms.headers.data(), ms.headers.size() * sizeof(ar::MaterialHeader)));
  sd.material_header_count = ms.count;
  sd.material_programs = static_cast<const uint32_t*>(put(ms.programs.data(), ms.programs.size() * 4));
  sd.material_constants =
      static_cast<const ar::ConstantBlock*>(put(ms.constants.data(), ms.constants.size() * sizeof(ar::ConstantBlock)));
  sd.material_pass_index = static_cast<const uint32_t*>(put(ms.pass_index.data(), ms.pass_index.size() * 4));
  void* scene_p = put(&sd, sizeof sd);

  ar::Rgb2SpecTable jh = tb.jh;
  jh.z_nodes = static_cast<const float*>(put(tb.jh.z_nodes, sizeof(float) * uint64_t(tb.jh.res)));
  jh.coeffs = static_cast<const float*>(put(tb.jh.coeffs, sizeof(float) * 9ull * uint64_t(tb.jh.res) * tb.jh.res * tb.jh.res));
  ar::SceneTables tables{};
  tables.rgb2spec = static_cast<const ar::Rgb2SpecTable*>(put(&jh, sizeof jh));
  tables.observer = ar::ObserverView{static_cast<const float*>(put(&tb.obs.cmf[0][0], sizeof tb.obs.cmf)),
                                     static_cast<const float*>(put(tb.obs.d65, sizeof tb.obs.d65))};
  void* tables_p = put(&tables, sizeof tables);
  DataBlob eblob;
  REQUIRE(find_data("tables/e_diel_refl.bin", &eblob));
  ar::EDielTable et{};
  REQUIRE(ar::e_diel_view(eblob.data, eblob.size, &et) == nullptr);
  et.e = static_cast<const float*>(put(et.e, 4ull * ar::kEDielN * ar::kEDielN * ar::kEDielN));
  void* e_diel_p = put(&et, sizeof et);

  // Path state from the sampler's independent stream (agree::unit), never a rand().
  std::vector<uint32_t> sample_id(kN), flags(kN), hit_prim(kN), hit_inst(kN);
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_SURFACE)), term(kN, uint8_t(ar::Term::ACTIVE)), ray_type(kN, 1);
  std::vector<ar::U16x6> bounces(kN);
  std::vector<ar::Real3> ray_d(kN);
  std::vector<ar::Real2> hit_uv(kN);
  std::vector<ar::Real> hit_t(kN, ar::Real(1));
  std::vector<ar::Spec> lambda_t(kN), beta(kN), r_u(kN), r_l(kN, ar::spec_fill(0.0f));
  std::vector<uint16_t> event_index(kN);
  std::vector<ar::Real3> ray_o(kN, ar::Real3{0, 0, 0});
  std::vector<ar::Real> eta_scale(kN, ar::Real(1)), prev_dist(kN, ar::Real(0));
  std::vector<uint32_t> prev_ng(kN, 0u), prev_ns(kN, 0u);
  for (uint32_t i = 0; i < kN; ++i) {
    sample_id[i] = i;
    hit_inst[i] = uint32_t(rnd(i, 30) * kInstances);
    hit_prim[i] = uint32_t(rnd(i, 31) * kTris);
    float u = rnd(i, 32), v = rnd(i, 33);
    if (u + v > 1.0f) u = 1.0f - u, v = 1.0f - v;
    hit_uv[i] = ar::Real2{u, v};
    ray_d[i] = ar::Real3{2.0f * rnd(i, 34) - 1.0f, 2.0f * rnd(i, 35) - 1.0f, 2.0f * rnd(i, 36) - 1.0f};
    for (int l = 0; l < ar::kLanes; ++l) {
      lambda_t[i].v[l] = 380.0f + 400.0f * rnd(i, uint32_t(l));
      beta[i].v[l] = 2.0f * rnd(i, 4u + uint32_t(l));
      r_u[i].v[l] = 0.5f + rnd(i, 40u + uint32_t(l));
    }
    event_index[i] = uint16_t(rnd(i, 44) * 6.0f);
    const double bad = agree::unit(i, 8);
    if (bad < 0.005) beta[i].v[int(rnd(i, 9) * 4.0f)] = std::numeric_limits<float>::quiet_NaN();
    else if (bad < 0.01) beta[i].v[int(rnd(i, 9) * 4.0f)] = std::numeric_limits<float>::infinity();
    flags[i] = ar::set_hero_lane(uint32_t(rnd(i, 10) * 4096.0f) << 9, uint32_t(rnd(i, 11) * 4.0f) & 3u) & ~ar::PATH_KIND;
    if (rnd(i, 37) < 0.5f) flags[i] |= ar::FRONT_FACE;  // stale side bits must be overwritten
    bounces[i] = ar::U16x6{{uint16_t(rnd(i, 12) < 0.75f ? 0 : 1 + int(rnd(i, 13) * 5.0f)), 0, 0, 0, 0, 0}};
  }
  ar::PathStateSoA soa{};
  soa.sample_id = static_cast<uint32_t*>(put(sample_id.data(), kN * 4));
  soa.next_stage = static_cast<uint8_t*>(put(next.data(), kN));
  soa.term = static_cast<uint8_t*>(put(term.data(), kN));
  soa.ray_type = static_cast<uint8_t*>(put(ray_type.data(), kN));
  soa.flags = static_cast<uint32_t*>(put(flags.data(), kN * 4));
  soa.bounces = static_cast<ar::U16x6*>(put(bounces.data(), kN * sizeof(ar::U16x6)));
  soa.ray_d = static_cast<ar::Real3*>(put(ray_d.data(), kN * sizeof(ar::Real3)));
  soa.lambda_t = static_cast<ar::Spec*>(put(lambda_t.data(), kN * sizeof(ar::Spec)));
  soa.beta = static_cast<ar::Spec*>(put(beta.data(), kN * sizeof(ar::Spec)));
  soa.hit_t = static_cast<ar::Real*>(put(hit_t.data(), kN * sizeof(ar::Real)));
  soa.hit_prim = static_cast<uint32_t*>(put(hit_prim.data(), kN * 4));
  soa.hit_inst = static_cast<uint32_t*>(put(hit_inst.data(), kN * 4));
  soa.hit_uv = static_cast<ar::Real2*>(put(hit_uv.data(), kN * sizeof(ar::Real2)));
  soa.event_index = static_cast<uint16_t*>(put(event_index.data(), kN * 2));
  soa.ray_o = static_cast<ar::Real3*>(put(ray_o.data(), kN * sizeof(ar::Real3)));
  soa.r_u = static_cast<ar::Spec*>(put(r_u.data(), kN * sizeof(ar::Spec)));
  soa.r_l = static_cast<ar::Spec*>(put(r_l.data(), kN * sizeof(ar::Spec)));
  soa.eta_scale = static_cast<ar::Real*>(put(eta_scale.data(), kN * sizeof(ar::Real)));
  soa.prev_ng = static_cast<uint32_t*>(put(prev_ng.data(), kN * 4));
  soa.prev_ns = static_cast<uint32_t*>(put(prev_ns.data(), kN * 4));
  soa.prev_dist = static_cast<ar::Real*>(put(prev_dist.data(), kN * sizeof(ar::Real)));
  void* soa_p = put(&soa, sizeof soa);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_p);
  kg.step_table = static_cast<const ar::StepTable*>(table_p);
  kg.main_queue = static_cast<const uint32_t*>(queue_p);
  kg.active_pixels = static_cast<uint32_t*>(list_p);
  kg.film = static_cast<ar::FilmData*>(film_p);
  kg.scene = static_cast<const ar::SceneData*>(scene_p);
  kg.tables = static_cast<const ar::SceneTables*>(tables_p);
  kg.e_diel = static_cast<const ar::EDielTable*>(e_diel_p);
  // No light table: NEE finds nothing (an empty sampler), and the emissive instances are no lights,
  // so a hit after a non-delta bounce takes pmf 0 and weight 1 / avg(r_u) (0004 D9; 074).
  const ar::LightSamplerView no_lights{};
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(put(&no_lights, sizeof no_lights));
  kg.session.width = kFilmW;
  kg.session.height = kFilmH;
  kg.session.samples_per_pass = kK;
  kg.session.seed = 3;
  kg.session.sampler_mode = ar::SAMPLER_INDEPENDENT;
  kg.session.max_bounces[0] = 8;
  void* kg_p = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_surface.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_p);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_surface", (kN + 255) / 256, 256, args);
  } else {
    // On the CPU device the uploaded copies are host memory the stage reads directly.
    cpu_stage_shade_surface(kg, step, 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  const ar::FilmData& fd = film.data();
  s.aov_position = fd.aov_position, s.aov_normal = fd.aov_normal, s.aov_uv = fd.aov_uv, s.aov_material = fd.aov_material;
  s.bounce_sum = fd.bounce_sum, s.plane_stride = fd.plane_stride;
  const auto offset = [&](const void* p) { return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film.buffer().ptr)); };
  s.slot_L.resize(size_t(kN) * 4);
  s.slot_flags.resize(kN);
  s.flags.resize(kN);
  s.next_stage.resize(kN);
  s.term.resize(kN);
  dev.download(s.slot_L.data(), film.buffer(), offset(fd.slot_L), s.slot_L.size() * 4);
  dev.download(s.slot_flags.data(), film.buffer(), offset(fd.slot_flags), kN * 4);
  // Path-state arrays: the buffers in upload order after the scene (sample_id is bufs[k0]).
  const auto find = [&](const void* p) -> const DeviceBuffer& {
    for (const DeviceBuffer& b : bufs)
      if (b.ptr == p) return b;
    FAIL("buffer not found");
    return bufs[0];
  };
  dev.download(s.flags.data(), find(soa.flags), 0, kN * 4);
  dev.download(s.next_stage.data(), find(soa.next_stage), 0, kN);
  dev.download(s.term.data(), find(soa.term), 0, kN);
  s.beta.resize(kN), s.r_l.resize(kN), s.ray_o.resize(kN), s.ray_d.resize(kN), s.bounces.resize(kN);
  s.event_index.resize(kN), s.ray_type.resize(kN);
  dev.download(s.beta.data(), find(soa.beta), 0, kN * sizeof(ar::Spec));
  dev.download(s.r_l.data(), find(soa.r_l), 0, kN * sizeof(ar::Spec));
  dev.download(s.ray_o.data(), find(soa.ray_o), 0, kN * sizeof(ar::Real3));
  dev.download(s.ray_d.data(), find(soa.ray_d), 0, kN * sizeof(ar::Real3));
  dev.download(s.bounces.data(), find(soa.bounces), 0, kN * sizeof(ar::U16x6));
  dev.download(s.event_index.data(), find(soa.event_index), 0, kN * 2);
  dev.download(s.ray_type.data(), find(soa.ray_type), 0, kN);
  const uint32_t n_count_planes = uint32_t(fd.term_hist) + ar::kTermCount;
  s.counts.resize(size_t(n_count_planes) * fd.plane_stride);
  dev.download(s.counts.data(), film.buffer(), offset(fd.count), s.counts.size() * 4);
  const int32_t last_add = std::max({fd.bounce_sum, fd.aov_position + 2, fd.aov_normal + 2, fd.aov_uv + 1, fd.aov_material});
  s.adds.resize(size_t(last_add + 1) * fd.plane_stride);
  dev.download(s.adds.data(), film.buffer(), offset(fd.add), s.adds.size() * 4);
  for (DeviceBuffer& b : bufs) dev.free(b);
  return s;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_surface/agreement") {
  const Tables tb;
  const Materials ms = build_materials(tb);
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr, tb, ms);
  const Snapshot g = run(*gpu, gpu.get(), tb, ms);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);

  // Integer fields: bitwise.
  CHECK(c.term == g.term);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.flags == g.flags);
  CHECK(c.slot_flags == g.slot_flags);
  CHECK(c.counts == g.counts);
  const auto plane = [](const Snapshot& s, int32_t first, uint32_t comps) {
    return std::vector<float>(s.adds.begin() + int64_t(first) * s.plane_stride,
                              s.adds.begin() + int64_t(first + int32_t(comps)) * s.plane_stride);
  };
  CHECK(plane(c, c.aov_material, 1) == plane(g, g.aov_material, 1));
  CHECK(plane(c, c.bounce_sum, 1) == plane(g, g.bounce_sum, 1));

  CHECK(c.event_index == g.event_index);
  CHECK(c.ray_type == g.ray_type);
  CHECK(std::memcmp(c.bounces.data(), g.bounces.data(), kN * sizeof(ar::U16x6)) == 0);

  // The fixture reaches every outcome: continued paths of both lobes, roulette, zero throughput,
  // valid ends, invalid ends from the corrupt program and the non-finite throughputs, both faces,
  // the default material (pass index 0 on some pixel).
  uint32_t ended = 0, continued = 0, glossy = 0, rr = 0, zero = 0, invalid = 0, front = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    ended += c.next_stage[i] == uint8_t(ar::Stage::TERMINATED);
    continued += c.next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST);
    glossy += c.next_stage[i] == uint8_t(ar::Stage::INTERSECT_CLOSEST) && c.ray_type[i] == ar::kRayTypeGlossy;
    rr += c.term[i] == uint8_t(ar::Term::RUSSIAN_ROULETTE);
    zero += c.term[i] == uint8_t(ar::Term::ZERO_THROUGHPUT);
    invalid += c.term[i] == uint8_t(ar::Term::INVALID);
    front += (c.flags[i] & ar::FRONT_FACE) != 0u;
  }
  CHECK(ended + continued == kN);
  CHECK(continued > kN / 10);
  CHECK(glossy > continued / 20);
  CHECK(glossy < continued);
  CHECK(rr > 100u);
  CHECK(zero > 100u);
  CHECK(invalid > 1000u);
  CHECK(invalid < kN / 2);
  CHECK(front > kN / 4);
  CHECK(front < 3 * kN / 4);
  std::vector<float> mat = plane(c, c.aov_material, 1);
  for (uint32_t want : {0u, 3u, 7u, 11u, 13u, 17u}) CHECK(std::count(mat.begin(), mat.end(), float(want)) > 0);

  // An invalid sample's slot is NaN on both devices: positions must match, then they are zeroed.
  std::vector<float> lc = c.slot_L, lg = g.slot_L;
  uint32_t nan_lanes = 0, nan_mismatch = 0;
  for (size_t i = 0; i < lc.size(); ++i) {
    const bool a = !(lc[i] == lc[i]), b = !(lg[i] == lg[i]);
    nan_lanes += a;
    nan_mismatch += a != b;
    if (a) lc[i] = 0.0f;
    if (b) lg[i] = 0.0f;
  }
  CHECK(nan_mismatch == 0u);
  CHECK(nan_lanes == 4u * invalid);
  double total = 0.0;
  for (float v : lc) total += double(v);
  CHECK(total > 1.0e4);  // emission really was added

  agree::Results rc, rg;
  rc.n = rg.n = kN;
  rc.stride = rg.stride = 4;
  rc.v.assign(lc.begin(), lc.end());
  rg.v.assign(lg.begin(), lg.end());
  const agree::Stats e = agree::compare(rg, rc, agree::Metric::REL_L2);
  CHECK(agree::report("shade_surface", "slot radiance nofma vs cpu float", e, agree::Cls::EVAL, agree::NOFMA_VS_CPU_FLOAT).pass);

  // Geometry AOVs: within 1e-5 of the larger magnitude (or absolute below 1).
  double worst = 0.0;
  for (const auto& pc : {std::make_pair(c.aov_position, 3u), std::make_pair(c.aov_normal, 3u), std::make_pair(c.aov_uv, 2u)}) {
    const std::vector<float> a = plane(c, pc.first, pc.second), b = plane(g, pc.first, pc.second);
    for (size_t i = 0; i < a.size(); ++i)
      worst = std::max(worst, std::fabs(double(a[i]) - double(b[i])) / std::max(1.0, std::fabs(double(a[i]))));
  }
  // The continued paths: throughput, r_l, direction and origin of the next ray.
  agree::Results bc, bg, lc2, lg2, dc, dg;
  for (agree::Results* r : {&bc, &bg, &lc2, &lg2}) r->n = continued, r->stride = 4;
  dc.n = dg.n = continued;
  dc.stride = dg.stride = 3;
  double worst_o = 0.0;
  for (uint32_t i = 0; i < kN; ++i) {
    if (c.next_stage[i] != uint8_t(ar::Stage::INTERSECT_CLOSEST)) continue;
    for (int l = 0; l < ar::kLanes; ++l) {
      bc.v.push_back(c.beta[i].v[l]), bg.v.push_back(g.beta[i].v[l]);
      lc2.v.push_back(c.r_l[i].v[l]), lg2.v.push_back(g.r_l[i].v[l]);
    }
    for (const float* p : {&c.ray_d[i].x, &c.ray_d[i].y, &c.ray_d[i].z}) dc.v.push_back(*p);
    for (const float* p : {&g.ray_d[i].x, &g.ray_d[i].y, &g.ray_d[i].z}) dg.v.push_back(*p);
    const float oc[3] = {c.ray_o[i].x, c.ray_o[i].y, c.ray_o[i].z}, og[3] = {g.ray_o[i].x, g.ray_o[i].y, g.ray_o[i].z};
    for (int k = 0; k < 3; ++k)
      worst_o = std::max(worst_o, std::fabs(double(oc[k]) - double(og[k])) / std::max(1.0, std::fabs(double(oc[k]))));
  }
  CHECK(agree::report("shade_surface", "beta nofma vs cpu float", agree::compare(bg, bc, agree::Metric::REL_L2),
                      agree::Cls::THROUGHPUT, agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("shade_surface", "r_l nofma vs cpu float", agree::compare(lg2, lc2, agree::Metric::REL_L2),
                      agree::Cls::THROUGHPUT, agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("shade_surface", "next direction nofma vs cpu float", agree::compare(dg, dc, agree::Metric::ANGLE),
                      agree::Cls::DIRECTION, agree::NOFMA_VS_CPU_FLOAT).pass);
  std::printf("shade_surface agreement: %u invalid, %u continued (%u glossy), %u roulette, %u zero of %u; worst geometry "
              "AOV difference %.3g, worst next-origin difference %.3g\n",
              invalid, continued, glossy, rr, zero, kN, worst, worst_o);
  CHECK(worst <= 1e-5);
  CHECK(worst_o <= 1e-5);
}

}  // TEST_SUITE
