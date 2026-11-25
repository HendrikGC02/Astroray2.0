// SPDX-License-Identifier: Apache-2.0
// NEE on the GPU against the CPU float stage (0009 d17 step iii, the per-stage fixture of 0002 D4):
// 10^5 synthetic surface hits on a random triangle soup (the default material) under a light table
// of every type: a point, a sphere, a rectangle and a disk lamp, the constant world, an emissive
// triangle with its Le in the record and one with a program emission (deferred Le: the emission
// entry runs at the light point), and a point lamp of a BLACKBODY colour. Arrival bounce counts 0-5
// under a total limit of 8, random throughputs and r_u, so the light pick, sample_li, the lobe eval and the balance weight all run.
// A second run replaces the constant world by an environment world (WORLD_ENV, 0004 D11) over a
// 32 x 32 table: its items carry the compensated pdf's MIS weight, ITEM_WORLD_LE and the lanes.
// The shadow items' stage, target and every integer field must be bit-equal; the contribution
// agrees to 0002 D12's throughput threshold, the segment direction to its direction threshold, the
// origin and tmax within 1e-5 relative (0009 d17 step iv's geometry tolerance).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D5, D12; 0009 d17; 0004 D1, D4.
// The production module is launched, so what runs is the shipped code.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/light/env_table.h"
#include "host/scene/scene_tables.h"
#include "host/material/compiler.h"
#include "host/spectral/blackbody.h"
#include "host/thread_pool.h"
#include "stages/shade_surface.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths
constexpr uint32_t kTris = 64;

float rnd(uint32_t i, uint32_t d) { return float(agree::unit(i, d)); }

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

// Material 0: two EMIT_LEs (a program emission entry); material 1 (= count): the default.
struct Materials {
  std::vector<ar::MaterialHeader> headers;
  std::vector<uint32_t> programs, pass_index;
  std::vector<ar::ConstantBlock> constants;
  uint32_t count = 0;
  void add(const MaterialOutput& m) {
    ar::MaterialHeader h = {};
    h.program_offset = uint32_t(programs.size()), h.const_offset = uint32_t(constants.size()), h.flags = m.flags;
    headers.push_back(h);
    programs.insert(programs.end(), m.compiled.program.begin(), m.compiled.program.end());
    constants.push_back(m.compiled.block);
    pass_index.push_back(0);
  }
};

Materials build_materials(const Tables& t) {
  std::vector<ar_graph_node> nodes(6);
  std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}, {3, 0, 5, 0}, {4, 0, 5, 1}};
  std::vector<float> f{0.2f, 0.4f, 0.6f, 1.5f, 1.0f, 1.0f, 1.0f, 0.5f};
  nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_first = 0, nodes[0].f32_count = 3;
  nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
  nodes[2].op = uint32_t(ar::Op::EMIT_LE);
  nodes[3].op = uint32_t(ar::Op::CONST_F32X3), nodes[3].f32_first = 4, nodes[3].f32_count = 3;
  nodes[4].op = uint32_t(ar::Op::CONST_F32), nodes[4].f32_first = 7, nodes[4].f32_count = 1;
  nodes[5].op = uint32_t(ar::Op::EMIT_LE);
  ar_graph_desc d = {};
  d.node_count = 6, d.link_count = 4, d.f32_count = 8, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
  Materials ms;
  const MaterialOutput mp = compile_material(d, t.jh, nullptr, nullptr);
  REQUIRE(!mp.magenta);
  REQUIRE((mp.flags & ar::CONSTANT_EMISSION) == 0u);  // the emission entry exists
  ms.add(mp);
  ms.count = 1;
  ms.add(compile_material(ar_graph_desc{}, t.jh, nullptr, nullptr));
  return ms;
}

struct Snapshot {
  std::vector<uint8_t> sh_next;
  std::vector<uint32_t> sh_target;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<ar::Real> sh_tmax;
  std::vector<ar::Spec> sh_contrib;
  std::vector<uint8_t> next_stage, term;
  std::vector<uint8_t> sh_flags;      // with an environment world only
  std::vector<ar::Spec> sh_lambda;
};

// The environment world's table: a smooth field with a bright patch, so the compensated table differs
// from a uniform one.
EnvTable env_fixture() {
  const uint32_t n = 32;
  std::vector<double> g(size_t(n) * n);
  for (uint32_t v = 0; v < n; ++v)
    for (uint32_t u = 0; u < n; ++u) g[size_t(v) * n + u] = 1.0 + 0.8 * std::sin(0.4 * u) * std::cos(0.3 * v) + (u / 4 == 3 && v / 4 == 5 ? 40.0 : 0.0);
  return build_env_table(g, n);
}

Snapshot run(Device& dev, ModuleDevice* gpu, const Tables& tb, const Materials& ms, const EnvTable* env = nullptr) {
  const Owner owner{1, 1};
  std::vector<DeviceBuffer> bufs;
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    bufs.push_back(b);
    return b.ptr;
  };
  const auto find = [&](const void* p) -> const DeviceBuffer& {
    for (const DeviceBuffer& b : bufs)
      if (b.ptr == p) return b;
    FAIL("buffer not found");
    return bufs[0];
  };
  FilmRequest req;
  req.max_width = kFilmW, req.max_height = kFilmH, req.slot_capacity = kN;
  req.statistics = true, req.termination_stats = true;
  Film film(dev, FilmLayout::build(req), owner);
  film.data().inv_radiance_scale = 0.5;  // s = 2: the deferred Le is multiplied by it once
  void* film_p = put(&film.data(), sizeof(ar::FilmData));

  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  void* queue_p = put(identity.data(), kN * 4);
  void* sh_queue_p = put(identity.data(), kN * 4);
  void* list_p = put(list.data(), list.size() * 4);
  const ar::StepEntry step{0, kN, 0};  // shadow_base 0, k = 1: item i owns shadow slot i
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_SURFACE)] = step;
  void* table_p = put(&table, sizeof table);

  // The soup: kTris triangles in [-1, 1]^3, normals near +z; instance 0 receives, instance 1 (the
  // same mesh 3 above) carries the program emission.
  std::vector<float> pos(9 * kTris), nrm(9 * kTris);
  std::vector<uint32_t> tri_v(3 * kTris);
  std::vector<int32_t> tri_m(kTris, 0);
  for (uint32_t t = 0; t < kTris; ++t) {
    for (uint32_t k = 0; k < 9; ++k) {
      pos[9 * t + k] = 2.0f * rnd(t, k) - 1.0f;
      nrm[9 * t + k] = (k % 3 == 2 ? 1.0f : 0.0f) + 0.6f * (rnd(t, 9 + k) - 0.5f);
    }
    for (uint32_t k = 0; k < 3; ++k) tri_v[3 * t + k] = 3 * t + k;
  }
  // Triangles 0 and 1 are also the emitters (instance 1): well-shaped, not random slivers. Arvo's
  // spherical-triangle sampling of a sliver seen near edge-on is ill-conditioned (the ulp
  // differences of acos / sinpi between the devices then move the sample by up to 7e-4 rad), which
  // is the sampler's conditioning, not an agreement defect of the stage.
  const float good[18] = {-0.6f, -0.6f, 0.0f, 0.6f, -0.5f, 0.1f, 0.0f, 0.6f, 0.05f,
                          0.2f, 0.1f, 0.3f, 1.0f, 0.2f, 0.2f, 0.5f, 0.9f, 0.25f};
  for (uint32_t k = 0; k < 18; ++k) pos[k] = good[k];
  ar::DeviceMeshView mv{};
  mv.positions = uint64_t(uintptr_t(put(pos.data(), pos.size() * 4)));
  mv.tri_vertex = uint64_t(uintptr_t(put(tri_v.data(), tri_v.size() * 4)));
  mv.tri_corners = uint64_t(uintptr_t(put(tri_v.data(), tri_v.size() * 4)));
  mv.corner_normals = uint64_t(uintptr_t(put(nrm.data(), nrm.size() * 4)));
  mv.tri_material = uint64_t(uintptr_t(put(tri_m.data(), tri_m.size() * 4)));
  mv.vertex_count = 3 * kTris, mv.triangle_count = kTris, mv.corner_count = 3 * kTris, mv.uv_layer_count = 0;
  std::vector<ar::DeviceInstanceRecord> inst(2);
  std::vector<uint32_t> pool{0};
  for (uint32_t j = 0; j < 2; ++j) {
    inst[j] = {};
    for (uint32_t k = 0; k < 12; ++k) inst[j].object_to_world[k] = k % 5 == 0 ? 1.0f : 0.0f;
    inst[j].light_index = ar::kNoLightIndex;
  }
  inst[1].object_to_world[11] = 3.0f;
  inst[1].material_offset = 0, inst[1].material_count = 1;
  ar::SceneData sd{};
  sd.instances = static_cast<const ar::DeviceInstanceRecord*>(put(inst.data(), inst.size() * sizeof(inst[0])));
  sd.instance_count = 2;
  sd.meshes = static_cast<const ar::DeviceMeshView*>(put(&mv, sizeof mv));
  sd.mesh_count = 1;
  sd.material_slots = static_cast<const uint32_t*>(put(pool.data(), pool.size() * 4));
  sd.material_headers = static_cast<const ar::MaterialHeader*>(put(ms.headers.data(), ms.headers.size() * sizeof(ar::MaterialHeader)));
  sd.material_header_count = ms.count;
  sd.material_programs = static_cast<const uint32_t*>(put(ms.programs.data(), ms.programs.size() * 4));
  sd.material_constants = static_cast<const ar::ConstantBlock*>(put(ms.constants.data(), ms.constants.size() * sizeof(ar::ConstantBlock)));
  sd.material_pass_index = static_cast<const uint32_t*>(put(ms.pass_index.data(), ms.pass_index.size() * 4));
  sd.material_spectra = nullptr, sd.material_spectra_count = 0;  // no material here holds a TABLE
  sd.medium_descs = nullptr, sd.medium_count = 0;                // no medium
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
  DataBlob sblob;
  REQUIRE(find_data("tables/e_spec.bin", &sblob));
  ar::ESpecTable st{};
  REQUIRE(ar::e_spec_view(sblob.data, sblob.size, &st) == nullptr);
  st.e = static_cast<const float*>(put(st.e, 4ull * ar::kESpecN * ar::kESpecN));
  void* e_spec_p = put(&st, sizeof st);

  // Every light type, uniform selection (an alias table of full bins). The last light is a point lamp
  // of a BLACKBODY colour, so the Planck spectrum runs through the light pick and sample_li on both
  // devices (indices of the others do not move).
  const ar::SpectrumRef white = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1));
  const ar::SpectrumRef planck = make_blackbody_ref(3000.0, 1.0, blackbody_y_normalised(3000.0), 1.0);
  const auto tri_world = [&](uint32_t t, int k) {
    return ar::Real3{pos[9 * t + 3 * k], pos[9 * t + 3 * k + 1], pos[9 * t + 3 * k + 2] + 3.0f};
  };
  std::vector<ar::LightRecord> lights{
      ar::light_make(ar::LightType::POINT, ar::Real3{0.3f, -0.2f, 2.5f}, 0.0f, 20.0f, white),
      ar::light_make(ar::LightType::SPHERE, ar::Real3{-1.5f, 0.5f, 2.0f}, 0.3f, 15.0f, white),
      ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{0.5f, 0.5f, 2.2f}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 1.0f,
                          0.6f, 25.0f, white),
      ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{-0.5f, -0.8f, 2.4f}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 0.4f,
                          0.0f, 18.0f, white),
      env ? ar::light_make_world(0.7f, white, ar::kLightWorldProgram) : ar::light_make_world(0.7f, white),
      ar::light_make_triangle(tri_world(0, 0), tri_world(0, 1), tri_world(0, 2), 1, 0, ar::kLightTwoSided, 3.0f, white),
      ar::light_make_triangle(tri_world(1, 0), tri_world(1, 1), tri_world(1, 2), 1, 1,
                              uint16_t(ar::kLightTwoSided | ar::kLightEmissionProgram), 0.0f, white),
      ar::light_make(ar::LightType::POINT, ar::Real3{-0.2f, 0.4f, 2.8f}, 0.0f, 20.0f, planck)};
  if (env) lights[4].type = uint8_t(ar::LightType::WORLD_ENV);
  const uint32_t nl = uint32_t(lights.size());
  std::vector<ar::AliasBin> bins;
  std::vector<uint32_t> bin_light;
  std::vector<float> pmf(nl, 1.0f / float(nl));
  for (uint32_t i = 0; i < nl; ++i) bins.push_back({1.0f, i}), bin_light.push_back(i);
  ar::LightSamplerView sampler{static_cast<const ar::AliasBin*>(put(bins.data(), bins.size() * sizeof(ar::AliasBin))),
                               static_cast<const uint32_t*>(put(bin_light.data(), nl * 4)),
                               static_cast<const float*>(put(pmf.data(), nl * 4)), nl, nl};
  void* lights_p = put(lights.data(), nl * sizeof(ar::LightRecord));
  void* sampler_p = put(&sampler, sizeof sampler);

  // Paths on instance 0, from the sampler's independent stream (agree::unit).
  std::vector<uint32_t> sample_id(kN), flags(kN), hit_prim(kN), hit_inst(kN, 0u), prev_ng(kN, 0u), prev_ns(kN, 0u);
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_SURFACE)), term(kN, uint8_t(ar::Term::ACTIVE)), ray_type(kN, 1);
  std::vector<ar::U16x6> bounces(kN);
  std::vector<ar::Real3> ray_d(kN), ray_o(kN, ar::Real3{0, 0, 0});
  std::vector<ar::Real2> hit_uv(kN);
  std::vector<ar::Real> hit_t(kN, 1.0f), eta_scale(kN, 1.0f), prev_dist(kN, 0.0f);
  std::vector<ar::Spec> lambda_t(kN), beta(kN), r_u(kN), r_l(kN, ar::spec_fill(1.0f));
  std::vector<uint16_t> event_index(kN);
  for (uint32_t i = 0; i < kN; ++i) {
    sample_id[i] = i;
    hit_prim[i] = uint32_t(rnd(i, 31) * kTris);
    float u = rnd(i, 32), v = rnd(i, 33);
    if (u + v > 1.0f) u = 1.0f - u, v = 1.0f - v;
    hit_uv[i] = ar::Real2{u, v};
    ray_d[i] = ar::Real3{2.0f * rnd(i, 34) - 1.0f, 2.0f * rnd(i, 35) - 1.0f, -0.2f - rnd(i, 36)};
    for (int l = 0; l < ar::kLanes; ++l) {
      lambda_t[i].v[l] = 380.0f + 400.0f * rnd(i, uint32_t(l));
      beta[i].v[l] = 2.0f * rnd(i, 4u + uint32_t(l));
      r_u[i].v[l] = 0.5f + rnd(i, 40u + uint32_t(l));
    }
    event_index[i] = uint16_t(rnd(i, 44) * 6.0f);
    flags[i] = ar::set_hero_lane(0u, uint32_t(rnd(i, 11) * 4.0f) & 3u);
    bounces[i] = ar::U16x6{{uint16_t(rnd(i, 12) * 6.0f), 0, 0, 0, 0, 0}};
  }
  ar::PathStateSoA soa{};
  soa.sample_id = static_cast<uint32_t*>(put(sample_id.data(), kN * 4));
  soa.next_stage = static_cast<uint8_t*>(put(next.data(), kN));
  soa.term = static_cast<uint8_t*>(put(term.data(), kN));
  soa.ray_type = static_cast<uint8_t*>(put(ray_type.data(), kN));
  soa.flags = static_cast<uint32_t*>(put(flags.data(), kN * 4));
  soa.bounces = static_cast<ar::U16x6*>(put(bounces.data(), kN * sizeof(ar::U16x6)));
  soa.ray_d = static_cast<ar::Real3*>(put(ray_d.data(), kN * sizeof(ar::Real3)));
  soa.ray_o = static_cast<ar::Real3*>(put(ray_o.data(), kN * sizeof(ar::Real3)));
  soa.lambda_t = static_cast<ar::Spec*>(put(lambda_t.data(), kN * sizeof(ar::Spec)));
  soa.beta = static_cast<ar::Spec*>(put(beta.data(), kN * sizeof(ar::Spec)));
  soa.r_u = static_cast<ar::Spec*>(put(r_u.data(), kN * sizeof(ar::Spec)));
  soa.r_l = static_cast<ar::Spec*>(put(r_l.data(), kN * sizeof(ar::Spec)));
  soa.hit_t = static_cast<ar::Real*>(put(hit_t.data(), kN * sizeof(ar::Real)));
  soa.hit_prim = static_cast<uint32_t*>(put(hit_prim.data(), kN * 4));
  soa.hit_inst = static_cast<uint32_t*>(put(hit_inst.data(), kN * 4));
  soa.hit_uv = static_cast<ar::Real2*>(put(hit_uv.data(), kN * sizeof(ar::Real2)));
  soa.event_index = static_cast<uint16_t*>(put(event_index.data(), kN * 2));
  soa.eta_scale = static_cast<ar::Real*>(put(eta_scale.data(), kN * sizeof(ar::Real)));
  soa.prev_ng = static_cast<uint32_t*>(put(prev_ng.data(), kN * 4));
  soa.prev_ns = static_cast<uint32_t*>(put(prev_ns.data(), kN * 4));
  soa.prev_dist = static_cast<ar::Real*>(put(prev_dist.data(), kN * sizeof(ar::Real)));
  void* soa_p = put(&soa, sizeof soa);

  // The shadow pool: every slot free, its fields poisoned so an unwritten field shows.
  std::vector<uint8_t> sh_next(kN, uint8_t(ar::Stage::SHADOW_TERMINATED));
  std::vector<uint32_t> sh_target(kN, ~0u);
  std::vector<ar::Real3> sh_zero3(kN, ar::Real3{0, 0, 0});
  std::vector<ar::Real> sh_tmax(kN, -1.0f);
  std::vector<ar::Spec> sh_contrib(kN, ar::spec_fill(0.0f));
  ar::ShadowItemSoA sh{};
  sh.next_stage = static_cast<uint8_t*>(put(sh_next.data(), kN));
  sh.target = static_cast<uint32_t*>(put(sh_target.data(), kN * 4));
  sh.ray_o = static_cast<ar::Real3*>(put(sh_zero3.data(), kN * sizeof(ar::Real3)));
  sh.ray_d = static_cast<ar::Real3*>(put(sh_zero3.data(), kN * sizeof(ar::Real3)));
  sh.tmax = static_cast<ar::Real*>(put(sh_tmax.data(), kN * sizeof(ar::Real)));
  sh.contribution = static_cast<ar::Spec*>(put(sh_contrib.data(), kN * sizeof(ar::Spec)));
  std::vector<uint8_t> sh_flags(kN, 0xFF);
  std::vector<ar::Spec> sh_lambda(kN, ar::spec_fill(-1.0f));
  ar::EnvTableView views[2] = {};
  if (env) {  // the env_world feature's fields, and the table as ENV_TABLE lays it out
    sh.item_flags = static_cast<uint8_t*>(put(sh_flags.data(), kN));
    sh.lambda_t = static_cast<ar::Spec*>(put(sh_lambda.data(), kN * sizeof(ar::Spec)));
    std::vector<float> floats;
    env_table_floats(*env, &floats);
    const float* p = static_cast<const float*>(put(floats.data(), floats.size() * 4));
    const EnvDistribution* d[2] = {&env->plain, &env->compensated};
    for (int k = 0; k < 2; ++k) {
      views[k].func = p, p += d[k]->func.size();
      views[k].cdf_u = p, p += d[k]->cdf_u.size();
      views[k].marginal_func = p, p += d[k]->marginal_func.size();
      views[k].marginal_cdf = p, p += d[k]->marginal_cdf.size();
      views[k].func_integral = d[k]->func_integral, views[k].n = d[k]->n;
    }
  }
  void* sh_p = put(&sh, sizeof sh);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_p);
  kg.shadow = static_cast<const ar::ShadowItemSoA*>(sh_p);
  kg.step_table = static_cast<const ar::StepTable*>(table_p);
  kg.main_queue = static_cast<const uint32_t*>(queue_p);
  kg.shadow_queue = static_cast<const uint32_t*>(sh_queue_p);
  kg.active_pixels = static_cast<uint32_t*>(list_p);
  kg.film = static_cast<ar::FilmData*>(film_p);
  kg.scene = static_cast<const ar::SceneData*>(scene_p);
  kg.tables = static_cast<const ar::SceneTables*>(tables_p);
  kg.e_diel = static_cast<const ar::EDielTable*>(e_diel_p);
  kg.e_spec = static_cast<const ar::ESpecTable*>(e_spec_p);
  kg.lights = static_cast<const ar::LightRecord*>(lights_p);
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(sampler_p);
  kg.world_light = 4;
  kg.env[0] = views[0], kg.env[1] = views[1];
  kg.session.features.env_world = env != nullptr;
  kg.session.width = kFilmW, kg.session.height = kFilmH, kg.session.samples_per_pass = kK;
  kg.session.seed = 9;
  kg.session.sampler_mode = ar::SAMPLER_INDEPENDENT;
  kg.session.max_bounces[0] = 8;
  void* kg_p = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_surface.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_p);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_surface", (kN + 255) / 256, 256, args);
  } else {
    cpu_stage_shade_surface(kg, step, 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  s.sh_next.resize(kN), s.sh_target.resize(kN), s.sh_o.resize(kN), s.sh_d.resize(kN), s.sh_tmax.resize(kN);
  s.sh_contrib.resize(kN), s.next_stage.resize(kN), s.term.resize(kN);
  dev.download(s.sh_next.data(), find(sh.next_stage), 0, kN);
  dev.download(s.sh_target.data(), find(sh.target), 0, kN * 4);
  dev.download(s.sh_o.data(), find(sh.ray_o), 0, kN * sizeof(ar::Real3));
  dev.download(s.sh_d.data(), find(sh.ray_d), 0, kN * sizeof(ar::Real3));
  dev.download(s.sh_tmax.data(), find(sh.tmax), 0, kN * sizeof(ar::Real));
  dev.download(s.sh_contrib.data(), find(sh.contribution), 0, kN * sizeof(ar::Spec));
  dev.download(s.next_stage.data(), find(soa.next_stage), 0, kN);
  dev.download(s.term.data(), find(soa.term), 0, kN);
  if (env) {
    s.sh_flags.resize(kN), s.sh_lambda.resize(kN);
    dev.download(s.sh_flags.data(), find(sh.item_flags), 0, kN);
    dev.download(s.sh_lambda.data(), find(sh.lambda_t), 0, kN * sizeof(ar::Spec));
  }
  for (DeviceBuffer& b : bufs) dev.free(b);
  return s;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("nee/agreement") {
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

  CHECK(c.sh_next == g.sh_next);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.term == g.term);
  // Items: emitted on a good share of the paths, toward every kind of light end (finite segments
  // to lamps and triangles, unbounded ones to the world), each targeting its own sample.
  uint32_t items = 0, unbounded = 0, invalid = 0;
  agree::Results cc, cg, dc, dg;
  double worst = 0.0;
  for (uint32_t i = 0; i < kN; ++i) {
    invalid += c.term[i] == uint8_t(ar::Term::INVALID);
    if (c.sh_next[i] != uint8_t(ar::Stage::INTERSECT_SHADOW)) continue;
    ++items;
    CHECK(c.sh_target[i] == i);
    CHECK(g.sh_target[i] == i);
    unbounded += c.sh_tmax[i] > 1e30f;
    for (int l = 0; l < ar::kLanes; ++l) cc.v.push_back(c.sh_contrib[i].v[l]), cg.v.push_back(g.sh_contrib[i].v[l]);
    for (const ar::Real3* d : {&c.sh_d[i]}) dc.v.insert(dc.v.end(), {d->x, d->y, d->z});
    for (const ar::Real3* d : {&g.sh_d[i]}) dg.v.insert(dg.v.end(), {d->x, d->y, d->z});
    const float a[4] = {c.sh_o[i].x, c.sh_o[i].y, c.sh_o[i].z, c.sh_tmax[i] > 1e30f ? 0.0f : c.sh_tmax[i]};
    const float b[4] = {g.sh_o[i].x, g.sh_o[i].y, g.sh_o[i].z, g.sh_tmax[i] > 1e30f ? 0.0f : g.sh_tmax[i]};
    for (int k = 0; k < 4; ++k) worst = std::max(worst, std::fabs(double(a[k]) - double(b[k])) / std::max(1.0, std::fabs(double(a[k]))));
  }
  CHECK(invalid == 0u);
  CHECK(items > kN / 4);
  CHECK(unbounded > items / 20);
  CHECK(unbounded < items);
  cc.n = cg.n = items, cc.stride = cg.stride = 4;
  dc.n = dg.n = items, dc.stride = dg.stride = 3;
  CHECK(agree::report("nee", "item contribution nofma vs cpu float", agree::compare(cg, cc, agree::Metric::REL_L2),
                      agree::Cls::THROUGHPUT, agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("nee", "item direction nofma vs cpu float", agree::compare(dg, dc, agree::Metric::ANGLE),
                      agree::Cls::DIRECTION, agree::NOFMA_VS_CPU_FLOAT).pass);
  std::printf("nee agreement: %u items of %u paths (%u to the world); worst origin / tmax difference %.3g\n", items, kN,
              unbounded, worst);
  CHECK(worst <= 1e-5);
}

// The same paths with the world replaced by an environment world: NEE toward it draws from the
// compensated table on both devices (0004 D11). Items toward the world carry ITEM_WORLD_LE and the
// path's lanes bit for bit; every other item carries flags 0; contributions and directions agree as
// above.
TEST_CASE("nee/environment agreement") {
  const Tables tb;
  const Materials ms = build_materials(tb);
  const EnvTable env = env_fixture();
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr, tb, ms, &env);
  const Snapshot g = run(*gpu, gpu.get(), tb, ms, &env);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(c.sh_next == g.sh_next);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.term == g.term);
  CHECK(c.sh_flags == g.sh_flags);
  uint32_t items = 0, world = 0, bad_flags = 0, bad_lanes = 0;
  agree::Results cc, cg, dc, dg;
  for (uint32_t i = 0; i < kN; ++i) {
    if (c.sh_next[i] != uint8_t(ar::Stage::INTERSECT_SHADOW)) continue;
    ++items;
    const bool to_world = c.sh_tmax[i] > 1e30f;
    world += to_world;
    bad_flags += c.sh_flags[i] != (to_world ? ar::ITEM_WORLD_LE : uint8_t(0));
    if (to_world)
      bad_lanes += std::memcmp(&c.sh_lambda[i], &g.sh_lambda[i], sizeof(ar::Spec)) != 0 || c.sh_lambda[i].v[0] < 0.0f;
    for (int l = 0; l < ar::kLanes; ++l) cc.v.push_back(c.sh_contrib[i].v[l]), cg.v.push_back(g.sh_contrib[i].v[l]);
    dc.v.insert(dc.v.end(), {c.sh_d[i].x, c.sh_d[i].y, c.sh_d[i].z});
    dg.v.insert(dg.v.end(), {g.sh_d[i].x, g.sh_d[i].y, g.sh_d[i].z});
  }
  CHECK(bad_flags == 0u);
  CHECK(bad_lanes == 0u);
  CHECK(world > items / 20);
  cc.n = cg.n = items, cc.stride = cg.stride = 4;
  dc.n = dg.n = items, dc.stride = dg.stride = 3;
  CHECK(agree::report("nee", "environment item contribution nofma vs cpu float", agree::compare(cg, cc, agree::Metric::REL_L2),
                      agree::Cls::THROUGHPUT, agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("nee", "environment item direction nofma vs cpu float", agree::compare(dg, dc, agree::Metric::ANGLE),
                      agree::Cls::DIRECTION, agree::NOFMA_VS_CPU_FLOAT).pass);
  std::printf("nee environment agreement: %u items of %u paths (%u to the environment)\n", items, kN, world);
}

}  // TEST_SUITE
