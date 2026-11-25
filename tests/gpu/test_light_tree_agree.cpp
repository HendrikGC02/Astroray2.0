// SPDX-License-Identifier: Apache-2.0
// The light tree and D10's floor in k_shade_surface on the GPU against the CPU float stage (0002 D4
// per-stage fixture, 0009 d17 step iii), and the tree against the alias-table hook on the GPU.
// 10^5 hits on a receiver quad under 64 rectangular lamps of the host light table (tree, radiometric
// and importance alias tables uploaded as the scene tables lay them out), light-only NEE
// (TEST_HOOK_NEE_LIGHT_ONLY: weight 1 / (pmf pdf), so every item is the whole direct-light estimate
// of its path). The no-FMA module and the CPU agree on the lamp each path draws (0002 D12's choice
// class: a draw at a float ulp from a branch threshold may fall the other way), and on the items of
// the paths that drew the same lamp (throughput and direction classes). On the GPU, the mean of 8
// launches with the tree equals that of 8 with the alias hook on independent streams (Welch, the
// gate family's z); the variance ratio alias / tree is printed.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0004 D10 (tree, floor, alias
// table as the oracle); Welch 1947.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "common/stat.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/light/light_builder.h"
#include "host/material/compiler.h"
#include "host/thread_pool.h"
#include "stages/shade_surface.h"

using namespace astroray;
using namespace astroray::host;
namespace st = astroray::stat;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths
constexpr uint32_t kLamps = 64;

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
  ar::ObserverView view() const { return ar::ObserverView{&obs.cmf[0][0], obs.d65}; }
};

// 64 rectangular lamps facing down on an 8 x 8 grid over [-2.1, 2.1]^2 at distinct heights (lamp i
// at z = 1.5 + i / 32), so a shadow segment's end names its lamp; powers vary. Sides 0.6 to 1.2:
// spherical-rectangle sampling of a lamp subtending a much smaller solid angle is ill-conditioned
// (its 1 / S from a sum of angles cancels; lamps of 0.1 to 0.5 at these distances gave p99.9 1e-3
// with the alias table as well as the tree), which is the lamp sampler's conditioning, not the stage's.
std::shared_ptr<const LightTable> lamps(const Tables& tb) {
  std::vector<LightInput> in;
  for (uint32_t i = 0; i < kLamps; ++i) {
    LightInput li;
    const float x = -2.1f + 0.6f * float(i % 8), y = -2.1f + 0.6f * float(i / 8), z = 1.5f + float(i) / 32.0f;
    const float a = 0.6f + 0.6f * rnd(i, 50), b = 0.6f + 0.6f * rnd(i, 51), s = 1.0f + 99.0f * rnd(i, 52);
    li.record = ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{x, y, z}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, a, b, s,
                                    ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, 1.0f));
    in.push_back(li);
  }
  return build_light_table(in, ar::SceneSphere{ar::Real3{0, 0, 1}, 6.0f}, tb.view(), nullptr);
}

struct Snapshot {
  std::vector<uint8_t> sh_next;
  std::vector<ar::Real3> sh_o, sh_d;
  std::vector<ar::Real> sh_tmax;
  std::vector<ar::Spec> sh_contrib;
};

// One launch of shade_surface over kN receiver hits (CPU stage when gpu is null).
Snapshot run_stage(Device& dev, ModuleDevice* gpu, const Tables& tb, const LightTable& lt, uint32_t hooks, uint32_t seed, uint16_t event) {
  const Owner owner{1, 1};
  std::vector<DeviceBuffer> bufs;
  const auto put = [&](const void* p, uint64_t bytes) -> void* {
    if (bytes == 0) return nullptr;
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
  void* film_p = put(&film.data(), sizeof(ar::FilmData));
  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  void* queue_p = put(identity.data(), kN * 4);
  void* sh_queue_p = put(identity.data(), kN * 4);
  void* list_p = put(list.data(), list.size() * 4);
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_SURFACE)] = step;
  void* table_p = put(&table, sizeof table);

  // The receiver: one quad of half-size 2 at z = 0 facing +z, the default material.
  const std::vector<float> pos{-2, -2, 0, 2, -2, 0, 2, 2, 0, -2, -2, 0, 2, 2, 0, -2, 2, 0};
  std::vector<float> nrm(18, 0.0f);
  for (int k = 0; k < 6; ++k) nrm[3 * k + 2] = 1.0f;
  const std::vector<uint32_t> tri_v{0, 1, 2, 3, 4, 5};
  const std::vector<int32_t> tri_m(2, 0);
  ar::DeviceMeshView mv{};
  mv.positions = uint64_t(uintptr_t(put(pos.data(), pos.size() * 4)));
  mv.tri_vertex = uint64_t(uintptr_t(put(tri_v.data(), tri_v.size() * 4)));
  mv.tri_corners = uint64_t(uintptr_t(put(tri_v.data(), tri_v.size() * 4)));
  mv.corner_normals = uint64_t(uintptr_t(put(nrm.data(), nrm.size() * 4)));
  mv.tri_material = uint64_t(uintptr_t(put(tri_m.data(), tri_m.size() * 4)));
  mv.vertex_count = 6, mv.triangle_count = 2, mv.corner_count = 6, mv.uv_layer_count = 0;
  ar::DeviceInstanceRecord inst{};
  for (uint32_t k = 0; k < 12; ++k) inst.object_to_world[k] = k % 5 == 0 ? 1.0f : 0.0f;
  inst.light_index = ar::kNoLightIndex;
  const MaterialOutput def = compile_material(ar_graph_desc{}, tb.jh, nullptr, nullptr);
  ar::MaterialHeader h = {};
  h.flags = def.flags;
  const std::vector<uint32_t> pass_index{0};
  ar::SceneData sd{};
  sd.instances = static_cast<const ar::DeviceInstanceRecord*>(put(&inst, sizeof inst));
  sd.instance_count = 1;
  sd.meshes = static_cast<const ar::DeviceMeshView*>(put(&mv, sizeof mv));
  sd.mesh_count = 1;
  sd.material_headers = static_cast<const ar::MaterialHeader*>(put(&h, sizeof h));
  sd.material_header_count = 0;  // the one header is the default material
  sd.material_programs = static_cast<const uint32_t*>(put(def.compiled.program.data(), def.compiled.program.size() * 4));
  sd.material_constants = static_cast<const ar::ConstantBlock*>(put(&def.compiled.block, sizeof(ar::ConstantBlock)));
  sd.material_pass_index = static_cast<const uint32_t*>(put(pass_index.data(), 4));
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
  ar::ESpecTable stt{};
  REQUIRE(ar::e_spec_view(sblob.data, sblob.size, &stt) == nullptr);
  stt.e = static_cast<const float*>(put(stt.e, 4ull * ar::kESpecN * ar::kESpecN));
  void* e_spec_p = put(&stt, sizeof stt);

  // The light table as the scene tables upload it (scene_tables.cpp: LIGHTS .. LIGHT_RAD_PMF).
  ar::LightSamplerView lv{};
  lv.bins = static_cast<const ar::AliasBin*>(put(lt.bins.data(), lt.bins.size() * sizeof(ar::AliasBin)));
  lv.bin_light = static_cast<const uint32_t*>(put(lt.bin_light.data(), lt.bin_light.size() * 4));
  lv.pmf = static_cast<const float*>(put(lt.pmf.data(), lt.pmf.size() * 4));
  lv.bin_count = uint32_t(lt.bins.size()), lv.light_count = uint32_t(lt.lights.size());
  lv.tree = ar::LightTreeView{static_cast<const ar::LightTreeNode*>(put(lt.tree_nodes.data(), lt.tree_nodes.size() * sizeof(ar::LightTreeNode))),
                              static_cast<const ar::LightTreeEmitter*>(put(lt.tree_emitters.data(), lt.tree_emitters.size() * sizeof(ar::LightTreeEmitter))),
                              uint32_t(lt.tree_nodes.size()), uint32_t(lt.tree_emitters.size())};
  lv.rad_bins = static_cast<const ar::AliasBin*>(put(lt.rad_bins.data(), lt.rad_bins.size() * sizeof(ar::AliasBin)));
  lv.rad_bin_light = static_cast<const uint32_t*>(put(lt.rad_bin_light.data(), lt.rad_bin_light.size() * 4));
  lv.rad_pmf = static_cast<const float*>(put(lt.rad_pmf.data(), lt.rad_pmf.size() * 4));
  lv.rad_bin_count = uint32_t(lt.rad_bins.size());
  void* lights_p = put(lt.lights.data(), lt.lights.size() * sizeof(ar::LightRecord));
  void* sampler_p = put(&lv, sizeof lv);

  // Camera hits on the receiver from the sampler's independent stream (agree::unit).
  std::vector<uint32_t> sample_id(kN), flags(kN), hit_prim(kN), hit_inst(kN, 0u), prev_ng(kN, 0u), prev_ns(kN, 0u);
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_SURFACE)), term(kN, uint8_t(ar::Term::ACTIVE)), ray_type(kN, 1);
  std::vector<ar::U16x6> bounces(kN, ar::U16x6{{0, 0, 0, 0, 0, 0}});
  std::vector<ar::Real3> ray_d(kN, ar::Real3{0, 0, -1}), ray_o(kN, ar::Real3{0, 0, 1});
  std::vector<ar::Real2> hit_uv(kN);
  std::vector<ar::Real> hit_t(kN, 1.0f), eta_scale(kN, 1.0f), prev_dist(kN, 0.0f);
  std::vector<ar::Spec> lambda_t(kN), beta(kN, ar::spec_fill(1.0f)), r_u(kN, ar::spec_fill(1.0f)), r_l(kN, ar::spec_fill(1.0f));
  std::vector<uint16_t> event_index(kN, event);
  for (uint32_t i = 0; i < kN; ++i) {
    sample_id[i] = i;
    hit_prim[i] = uint32_t(rnd(i, 31) * 2.0f);
    float u = rnd(i, 32), v = rnd(i, 33);
    if (u + v > 1.0f) u = 1.0f - u, v = 1.0f - v;
    hit_uv[i] = ar::Real2{u, v};
    for (int l = 0; l < ar::kLanes; ++l) lambda_t[i].v[l] = 380.0f + 400.0f * rnd(i, uint32_t(l));
    flags[i] = ar::set_hero_lane(0u, uint32_t(rnd(i, 11) * 4.0f) & 3u);
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
  kg.world_light = ar::kNoLight;
  kg.session.width = kFilmW, kg.session.height = kFilmH, kg.session.samples_per_pass = kK;
  kg.session.seed = seed;
  kg.session.test_hooks = hooks;
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
  s.sh_next.resize(kN), s.sh_o.resize(kN), s.sh_d.resize(kN), s.sh_tmax.resize(kN), s.sh_contrib.resize(kN);
  dev.download(s.sh_next.data(), find(sh.next_stage), 0, kN);
  dev.download(s.sh_o.data(), find(sh.ray_o), 0, kN * sizeof(ar::Real3));
  dev.download(s.sh_d.data(), find(sh.ray_d), 0, kN * sizeof(ar::Real3));
  dev.download(s.sh_tmax.data(), find(sh.tmax), 0, kN * sizeof(ar::Real));
  dev.download(s.sh_contrib.data(), find(sh.contribution), 0, kN * sizeof(ar::Spec));
  for (DeviceBuffer& b : bufs) dev.free(b);
  return s;
}

// The lamp a path's item ends on (its height names it), or -1 without an item.
double lamp_of(const Snapshot& s, uint32_t i) {
  if (s.sh_next[i] != uint8_t(ar::Stage::INTERSECT_SHADOW)) return -1.0;
  const double z = double(s.sh_o[i].z) + double(s.sh_d[i].z) * double(s.sh_tmax[i]);
  return std::round((z - 1.5) * 32.0);
}

// The per-path scalar of the Welch test: the item's lane mean, 0 without an item.
double value(const Snapshot& s, uint32_t i) {
  if (s.sh_next[i] != uint8_t(ar::Stage::INTERSECT_SHADOW)) return 0.0;
  double x = 0.0;
  for (int k = 0; k < ar::kLanes; ++k) x += double(s.sh_contrib[i].v[k]) / ar::kLanes;
  return x;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("light_tree_agree/tree nofma vs cpu") {
  const st::Run run = st::begin("light_tree_agree/tree nofma vs cpu");
  const Tables tb;
  const auto lt = lamps(tb);
  REQUIRE(lt->tree_nodes.size() == 2 * kLamps - 1);
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const uint32_t tree = ar::TEST_HOOK_NEE_LIGHT_ONLY, alias = tree | ar::TEST_HOOK_LIGHT_ALIAS;
  const Snapshot c = run_stage(cpu, nullptr, tb, *lt, tree, 9, 0);
  const Snapshot g = run_stage(*gpu, gpu.get(), tb, *lt, tree, 9, 0);

  // The lamp each path drew, then the items of the paths that drew the same lamp.
  agree::Results lc, lg, cc, cg, dc, dg;
  uint32_t same = 0, items = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    const double a = lamp_of(c, i), b = lamp_of(g, i);
    lc.v.push_back(a), lg.v.push_back(b);
    items += a >= 0.0;
    if (a < 0.0 || a != b) continue;
    ++same;
    for (int l = 0; l < ar::kLanes; ++l) cc.v.push_back(c.sh_contrib[i].v[l]), cg.v.push_back(g.sh_contrib[i].v[l]);
    dc.v.insert(dc.v.end(), {c.sh_d[i].x, c.sh_d[i].y, c.sh_d[i].z});
    dg.v.insert(dg.v.end(), {g.sh_d[i].x, g.sh_d[i].y, g.sh_d[i].z});
  }
  CHECK(items > kN * 9 / 10);
  lc.n = lg.n = kN, lc.stride = lg.stride = 1;
  cc.n = cg.n = same, cc.stride = cg.stride = 4;
  dc.n = dg.n = same, dc.stride = dg.stride = 3;
  CHECK(agree::report("light_tree", "lamp choice nofma vs cpu float", agree::compare(lg, lc, agree::Metric::EQUAL), agree::Cls::CHOICE,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("light_tree", "item contribution nofma vs cpu float", agree::compare(cg, cc, agree::Metric::REL_L2),
                      agree::Cls::THROUGHPUT, agree::NOFMA_VS_CPU_FLOAT).pass);
  CHECK(agree::report("light_tree", "item direction nofma vs cpu float", agree::compare(dg, dc, agree::Metric::ANGLE),
                      agree::Cls::DIRECTION, agree::NOFMA_VS_CPU_FLOAT).pass);

  // The tree against the alias hook on the GPU: 8 launches each, independent seeds and event indices.
  constexpr int kLaunches = 8;
  double mean[2], var[2];
  for (int mode = 0; mode < 2; ++mode) {
    double s1 = 0.0, s2 = 0.0;
    for (int k = 0; k < kLaunches; ++k) {
      const Snapshot r = run_stage(*gpu, gpu.get(), tb, *lt, mode == 0 ? tree : alias, uint32_t(run.seed) + uint32_t(1000 * mode + k), uint16_t(k));
      for (uint32_t i = 0; i < kN; ++i) {
        const double x = value(r, i);
        REQUIRE(std::isfinite(x));
        s1 += x, s2 += x * x;
      }
    }
    const double m = double(kN) * kLaunches;
    mean[mode] = s1 / m;
    var[mode] = (s2 / m - mean[mode] * mean[mode]) * m / (m - 1.0);
  }
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
  const double m = double(kN) * kLaunches, se = std::sqrt(var[0] / m + var[1] / m), z = (mean[0] - mean[1]) / se;
  run.family->note_check();
  std::printf("light_tree_agree 64 lamps cuda: tree %.6g, alias %.6g, se %.3g, z %+.2f (limit %.2f); variance ratio alias / tree %.3f; "
              "%u of %u items on the same lamp\n",
              mean[0], mean[1], se, z, run.family->z(), var[1] / var[0], same, items);
  INFO(st::reproduce(run));
  CHECK(std::fabs(z) < run.family->z());
}

}  // TEST_SUITE("gate")
