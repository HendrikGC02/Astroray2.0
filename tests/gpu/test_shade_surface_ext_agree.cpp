// SPDX-License-Identifier: Apache-2.0
// shade_surface_ext on the GPU (the FMA-off production module) against the CPU float stage on the same
// states (0002 D4, D12; 0009 d17 step iii): 2^14 hits on a hand-built quad under a reflective GRATING,
// with the materials, their ExtParams and the TableArena as the host store lays them out. The store
// holds two distinct gratings and the quad uses the second, so the kernel reads its table at a
// non-zero arena offset. Directions arrive from the upper hemisphere, lanes at random wavelengths, so
// order 0 (four lanes), the other orders (secondaries terminated) and evanescent orders all occur.
// Integer fields (term, next stage, flags, ray type, counters, event index) must be bit-equal; lambda_t
// too; beta within 1e-6 relative and the next direction within 1e-6 (both sides compute the same
// float expressions without contraction). On both devices each surviving path's throughput factor is
// item 22's eta_m / p_m, recomputed on the host from grating B's table at its own offset, for the order
// its direction names.
// The render case replays scenes/ext_grating_card (out/scenes, built by the scenes step) through
// astroray_cli at 32 x 32 and 4 spp on the CPU and the CUDA device: every display pixel of the film dump
// is finite and the image is not black (188 owns the grating's image truth).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0005 items 16, 17, 21-23; 0009 d17.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/material/grating_authored.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/scene.h"
#include "host/thread_pool.h"
#include "stages/shade_surface_ext.h"

#include <windows.h>

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kN = 1u << 14;

float rnd(uint32_t i, uint32_t d) { return float(agree::unit(i, d)); }

ar_material_desc grating(float depth_nm) {
  GratingAuthored g = {};
  g.pitch_nm = 1600.0f, g.depth_nm = depth_nm, g.coating_n = 1.0f, g.coating_k = 6.0f, g.m_min = -2, g.m_max = 2;
  ar_material_desc d = {};
  d.ext_kind = uint32_t(ar::ExtKind::GRATING);
  std::memcpy(d.ext_constants, &g, sizeof g);
  return d;
}

// The host tables the stage reads: the store's arrays for grating A (key 1) and B (key 2), and the
// observer and RGB-to-spectrum tables.
struct Inputs {
  std::shared_ptr<const MaterialArrays> mats;
  std::vector<float> table_b;  // grating B's table as compile_extension builds it
  ObserverTables obs;
  ar::Rgb2SpecTable jh = {};
  Inputs() {
    ContextState ctx;
    Scene scene(ctx, SceneConfig{2, {}});
    auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
    auto e = scene.begin_edit();
    REQUIRE(e);
    const ar_material_desc a = grating(90.0f), b = grating(120.0f);
    REQUIRE(store->set_material(*e, 1, &a) == AR_OK);
    REQUIRE(store->set_material(*e, 2, &b) == AR_OK);
    uint64_t gen = 0;
    REQUIRE(e->commit(&gen) == AR_OK);
    store->end_edit();
    scene.generations().wait_idle();
    mats = store->arrays();
    const ar_graph_desc none = {};
    table_b = compile_extension(b, none, store->table()).compiled.ext_table;
    REQUIRE(mats->ext_params.size() == 2u);
    REQUIRE(ar::grating_params(mats->ext_params[1]).table != 0u);
    DataBlob blob;
    REQUIRE(find_data("spectral/observer.bin", &blob));
    REQUIRE(parse_observer(blob.data, blob.size, &obs).empty());
    REQUIRE(find_data("spectral/rec709.jh", &blob));
    REQUIRE(ar::jh_view(blob.data, blob.size, &jh) == nullptr);
  }
};

// The arrival states, drawn from the sampler's independent stream (agree::unit).
struct States {
  std::vector<uint32_t> hit_prim, flags;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real3> ray_d;
  std::vector<ar::Spec> lambda_t, beta, r_u;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  States() : hit_prim(kN), flags(kN), hit_uv(kN), ray_d(kN), lambda_t(kN), beta(kN), r_u(kN), event_index(kN), bounces(kN) {
    for (uint32_t i = 0; i < kN; ++i) {
      hit_prim[i] = rnd(i, 1) < 0.5f ? 0u : 1u;
      float u = rnd(i, 2), v = rnd(i, 3);
      if (u + v > 1.0f) u = 1.0f - u, v = 1.0f - v;
      hit_uv[i] = ar::Real2{u, v};
      // Downward rays, from grazing to normal incidence.
      ray_d[i] = ar::Real3{2.0f * rnd(i, 4) - 1.0f, 2.0f * rnd(i, 5) - 1.0f, -0.05f - rnd(i, 6)};
      for (int l = 0; l < ar::kLanes; ++l) {
        lambda_t[i].v[l] = 380.0f + 400.0f * rnd(i, 10u + uint32_t(l));
        beta[i].v[l] = 0.25f + rnd(i, 20u + uint32_t(l));
        r_u[i].v[l] = 0.5f + rnd(i, 30u + uint32_t(l));
      }
      event_index[i] = uint16_t(rnd(i, 7) * 6.0f);
      flags[i] = ar::set_hero_lane(0u, uint32_t(rnd(i, 8) * 4.0f) & 3u);
      bounces[i] = ar::U16x6{{0, 0, 0, 0, 0, 0}};  // first bounce: Russian roulette starts at depth 2
    }
  }
};

struct Snapshot {
  std::vector<uint32_t> flags;
  std::vector<uint8_t> next_stage, term, ray_type;
  std::vector<ar::Spec> beta, lambda_t;
  std::vector<ar::Real3> ray_d;
  std::vector<ar::U16x6> bounces;
  std::vector<uint16_t> event_index;
};

Snapshot run(Device& dev, ModuleDevice* gpu, const Inputs& in, const States& st) {
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
  req.max_width = kN;
  req.max_height = 1;
  req.slot_capacity = kN;
  req.statistics = true;
  req.termination_stats = true;
  Film film(dev, FilmLayout::build(req), owner);
  film.data().inv_radiance_scale = 1.0;
  void* film_p = put(&film.data(), sizeof(ar::FilmData));
  std::vector<uint32_t> identity(kN);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  void* queue_p = put(identity.data(), kN * 4);
  void* list_p = put(identity.data(), kN * 4);
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_SURFACE_EXT)] = step;
  void* table_p = put(&table, sizeof table);

  // The quad of half-size 1 at z = 0 facing +z, both triangles in slot 0, its one instance on grating B.
  const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
  const std::vector<float> nrm{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const std::vector<uint32_t> tri{0, 1, 2, 3, 4, 5};
  const std::vector<int32_t> tri_m{0, 0};
  ar::DeviceMeshView mv{};
  mv.positions = uint64_t(uintptr_t(put(pos.data(), pos.size() * 4)));
  mv.tri_vertex = uint64_t(uintptr_t(put(tri.data(), tri.size() * 4)));
  mv.tri_corners = uint64_t(uintptr_t(put(tri.data(), tri.size() * 4)));
  mv.corner_normals = uint64_t(uintptr_t(put(nrm.data(), nrm.size() * 4)));
  mv.tri_material = uint64_t(uintptr_t(put(tri_m.data(), tri_m.size() * 4)));
  mv.vertex_count = 6, mv.triangle_count = 2, mv.corner_count = 6, mv.uv_layer_count = 0;
  ar::DeviceInstanceRecord inst{};
  for (uint32_t k = 0; k < 12; ++k) inst.object_to_world[k] = k % 5 == 0 ? 1.0f : 0.0f;
  inst.light_index = ar::kNoLightIndex;
  inst.material_offset = 0, inst.material_count = 1, inst.mask = 1u;
  const std::vector<uint32_t> pool{1};  // header 1: grating B
  const MaterialArrays& m = *in.mats;
  ar::SceneData sd{};
  sd.instances = static_cast<const ar::DeviceInstanceRecord*>(put(&inst, sizeof inst));
  sd.instance_count = 1;
  sd.meshes = static_cast<const ar::DeviceMeshView*>(put(&mv, sizeof mv));
  sd.mesh_count = 1;
  sd.material_slots = static_cast<const uint32_t*>(put(pool.data(), 4));
  sd.material_headers = static_cast<const ar::MaterialHeader*>(put(m.headers.data(), m.headers.size() * sizeof(ar::MaterialHeader)));
  sd.material_header_count = uint32_t(m.keys.size());
  sd.material_programs = static_cast<const uint32_t*>(put(m.programs.data(), m.programs.size() * 4));
  sd.material_constants = static_cast<const ar::ConstantBlock*>(put(m.constants.data(), m.constants.size() * sizeof(ar::ConstantBlock)));
  sd.material_pass_index = static_cast<const uint32_t*>(put(m.pass_index.data(), m.pass_index.size() * 4));
  sd.ext_params = static_cast<const ar::ExtParams*>(put(m.ext_params.data(), m.ext_params.size() * sizeof(ar::ExtParams)));
  sd.ext_param_count = uint32_t(m.ext_params.size());
  sd.table_arena = static_cast<const uint8_t*>(put(m.table_arena.data(), m.table_arena.size()));
  sd.table_arena_bytes = uint32_t(m.table_arena.size());
  void* scene_p = put(&sd, sizeof sd);
  ar::Rgb2SpecTable jh = in.jh;
  jh.z_nodes = static_cast<const float*>(put(in.jh.z_nodes, sizeof(float) * uint64_t(in.jh.res)));
  jh.coeffs = static_cast<const float*>(put(in.jh.coeffs, sizeof(float) * 9ull * uint64_t(in.jh.res) * in.jh.res * in.jh.res));
  ar::SceneTables tables{};
  tables.rgb2spec = static_cast<const ar::Rgb2SpecTable*>(put(&jh, sizeof jh));
  tables.observer = ar::ObserverView{static_cast<const float*>(put(&in.obs.cmf[0][0], sizeof in.obs.cmf)),
                                     static_cast<const float*>(put(in.obs.d65, sizeof in.obs.d65))};
  void* tables_p = put(&tables, sizeof tables);

  std::vector<uint32_t> sample_id(kN), flags = st.flags, hit_prim = st.hit_prim, hit_inst(kN, 0u), prev(kN, 0u);
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_SURFACE_EXT)), term(kN, uint8_t(ar::Term::ACTIVE)), ray_type(kN, 1);
  std::vector<ar::U16x6> bounces = st.bounces;
  std::vector<ar::Real3> ray_d = st.ray_d, ray_o(kN, ar::Real3{0, 0, 0});
  std::vector<ar::Real2> hit_uv = st.hit_uv;
  std::vector<ar::Real> hit_t(kN, ar::Real(1)), eta_scale(kN, ar::Real(1)), prev_dist(kN, ar::Real(0));
  std::vector<ar::Spec> lambda_t = st.lambda_t, beta = st.beta, r_u = st.r_u, r_l(kN, ar::spec_fill(0.0f));
  std::vector<uint16_t> event_index = st.event_index;
  for (uint32_t i = 0; i < kN; ++i) sample_id[i] = i;
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
  soa.prev_ng = static_cast<uint32_t*>(put(prev.data(), kN * 4));
  soa.prev_ns = static_cast<uint32_t*>(put(prev.data(), kN * 4));
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
  // No E tables: the Ext identity never calls the lobe stack. No light table: NEE finds nothing.
  const ar::LightSamplerView no_lights{};
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(put(&no_lights, sizeof no_lights));
  kg.session.width = kN;
  kg.session.height = 1;
  kg.session.samples_per_pass = 1;
  kg.session.seed = 5;
  kg.session.sampler_mode = ar::SAMPLER_INDEPENDENT;
  kg.session.max_bounces[0] = 8;
  void* kg_p = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_surface_ext.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_p);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_surface_ext", (kN + 255) / 256, 256, args);
  } else {
    cpu_stage_shade_surface_ext(kg, step, 0, kN);  // the uploaded copies are host memory
  }
  dev.synchronize();

  Snapshot s;
  s.flags.resize(kN), s.next_stage.resize(kN), s.term.resize(kN), s.ray_type.resize(kN);
  s.beta.resize(kN), s.lambda_t.resize(kN), s.ray_d.resize(kN), s.bounces.resize(kN), s.event_index.resize(kN);
  dev.download(s.flags.data(), find(soa.flags), 0, kN * 4);
  dev.download(s.next_stage.data(), find(soa.next_stage), 0, kN);
  dev.download(s.term.data(), find(soa.term), 0, kN);
  dev.download(s.ray_type.data(), find(soa.ray_type), 0, kN);
  dev.download(s.beta.data(), find(soa.beta), 0, kN * sizeof(ar::Spec));
  dev.download(s.lambda_t.data(), find(soa.lambda_t), 0, kN * sizeof(ar::Spec));
  dev.download(s.ray_d.data(), find(soa.ray_d), 0, kN * sizeof(ar::Real3));
  dev.download(s.bounces.data(), find(soa.bounces), 0, kN * sizeof(ar::U16x6));
  dev.download(s.event_index.data(), find(soa.event_index), 0, kN * 2);
  for (DeviceBuffer& b : bufs) dev.free(b);
  return s;
}

// Paths of `s` whose throughput factor is not item 22's eta_m / p_m to 1e-5 relative: the order m from
// the outgoing direction at lambda_t[0] (N = +z and the default tangent x make the frame the identity),
// p_m the lane mean of eta over the propagating orders over its sum, eta from the table the compiler builds
// from grating B's authored block (independent of its arena offset); every lane for order 0, lane 0
// (copied) otherwise.
uint32_t factor_mismatches(const Inputs& in, const States& st, const Snapshot& s) {
  const MaterialArrays& m = *in.mats;
  const ar::GratingParams g = ar::grating_params(m.ext_params[ar::ext_index(m.headers[1])]);
  REQUIRE(in.table_b.size() == size_t(g.m_max - g.m_min + 1) * ar::kGratingCosBins * ar::kGratingWavelengths);
  const ar::GratingTable tab{in.table_b.data(), g.m_min, g.m_max - g.m_min + 1};
  const ar::Spec one = ar::spec_fill(ar::Real(1));
  uint32_t bad = 0, checked = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    if (s.term[i] != uint8_t(ar::Term::ACTIVE)) continue;
    const ar::Real3 wo = -ar::normalize(st.ray_d[i]);
    ar::Lambda lam;
    for (int l = 0; l < ar::kLanes; ++l) lam.nm[l] = st.lambda_t[i].v[l];
    const ar::Real cos_i = std::min(std::fabs(wo.z), ar::Real(1));
    double sum = 0.0;
    for (int k = g.m_min; k <= g.m_max; ++k) sum += double(ar::grating_order_mean(g, tab, wo, k, lam, one, one, cos_i));
    int order = 99;
    for (int k = g.m_min; k <= g.m_max && order == 99; ++k) {
      ar::Real3 d{};
      if (ar::grating_order_direction(wo, k, lam.nm[0], ar::Real(g.pitch_nm), false, ar::Real(1), ar::Real(1), &d) &&
          std::fabs(double(d.x - s.ray_d[i].x)) + std::fabs(double(d.y - s.ray_d[i].y)) + std::fabs(double(d.z - s.ray_d[i].z)) < 1e-5)
        order = k;
    }
    if (order == 99 || !(sum > 0.0)) {
      ++bad;
      continue;
    }
    const double p = double(ar::grating_order_mean(g, tab, wo, order, lam, one, one, cos_i)) / sum;
    for (int l = 0; l < (order == 0 ? ar::kLanes : 1); ++l) {
      const double eta = double(ar::grating_lane_eta(g, tab, wo, order, lam.nm[l], ar::Real(1), ar::Real(1), cos_i));
      const double want = double(st.beta[i].v[order == 0 ? l : 0]) * eta / p;
      ++checked;
      if (!(std::fabs(double(s.beta[i].v[l]) - want) <= 1e-5 * want)) ++bad;
    }
  }
  std::printf("shade_surface_ext agree: %u throughput factors checked against eta / p_m, %u off\n", checked, bad);
  return checked > kN / 4u ? bad : ~0u;
}

// The display.0-2 mean planes of an astroray_cli film dump (FITS: 2880 B blocks of 80 B cards, one image
// HDU per plane, big-endian float data), as doubles.
std::vector<std::vector<double>> display_planes(const std::filesystem::path& fits) {
  std::ifstream f(fits, std::ios::binary);
  const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::vector<double>> planes(3);
  size_t at = 0;
  while (at + 2880 <= b.size()) {
    long long bitpix = 0, naxis = 0, n1 = 1, n2 = 1;
    std::string extname;
    size_t card = at;
    bool end = false;
    for (; card + 80 <= b.size() && !end; card += 80) {
      const std::string c(reinterpret_cast<const char*>(b.data() + card), 80);
      const std::string key = c.substr(0, 8);
      const auto value = [&] { return c.substr(10, 70); };
      if (key == "END     ") end = true;
      else if (key == "BITPIX  ") bitpix = std::atoll(value().c_str());
      else if (key == "NAXIS   ") naxis = std::atoll(value().c_str());
      else if (key == "NAXIS1  ") n1 = std::atoll(value().c_str());
      else if (key == "NAXIS2  ") n2 = std::atoll(value().c_str());
      else if (key == "EXTNAME ") {
        const std::string v = value();
        const size_t q0 = v.find('\''), q1 = v.find('\'', q0 + 1);
        extname = v.substr(q0 + 1, q1 - q0 - 1);
        while (!extname.empty() && extname.back() == ' ') extname.pop_back();
      }
    }
    if (!end) break;
    const size_t data = (card - at + 2879) / 2880 * 2880 + at;
    const size_t elem = size_t(bitpix < 0 ? -bitpix : bitpix) / 8, count = naxis == 0 ? 0 : size_t(n1 * n2);
    for (int c = 0; c < 3; ++c) {
      if (extname != "display." + std::to_string(c) || data + count * elem > b.size()) continue;
      for (size_t i = 0; i < count; ++i) {
        uint64_t w = 0;
        for (size_t k = 0; k < elem; ++k) w = (w << 8) | b[data + i * elem + k];
        double v = 0.0;
        if (bitpix == -32) {
          float x;
          const uint32_t w32 = uint32_t(w);
          std::memcpy(&x, &w32, 4);
          v = x;
        } else if (bitpix == -64) {
          std::memcpy(&v, &w, 8);
        } else {
          v = std::nan("");  // not a float plane: fails the finiteness check
        }
        planes[size_t(c)].push_back(v);
      }
    }
    at = data + (count * elem + 2879) / 2880 * 2880;
  }
  return planes;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_surface_ext/grating card renders on both devices") {
  char exe[MAX_PATH] = {};
  REQUIRE(GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0);
  const std::filesystem::path bin = std::filesystem::path(exe).parent_path();
  const std::filesystem::path cli = bin / "astroray_cli.exe";
  const std::filesystem::path rec = bin.parent_path() / "scenes" / "ext_grating_card.arrec";
  REQUIRE_MESSAGE(std::filesystem::exists(cli), cli.string() << " missing: run tools/build.ps1");
  REQUIRE_MESSAGE(std::filesystem::exists(rec), rec.string() << " missing: run tools/check.ps1 (scenes)");
  for (const char* device : {"cpu", "cuda"}) {
    INFO("device " << device);
    const std::filesystem::path fits = std::filesystem::temp_directory_path() / ("astroray_grating_card_" + std::string(device) + ".fits");
    std::filesystem::remove(fits);
    const std::string cmd = "\"\"" + cli.string() + "\" render \"" + rec.string() + "\" -o \"" + fits.string() + "\" --device " +
                            device + " --spp 4 --size 32x32 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    const int rc = _pclose(p);
    REQUIRE_MESSAGE(rc == 0, out);
    const std::vector<std::vector<double>> planes = display_planes(fits);
    double sum = 0.0, peak = 0.0;
    uint32_t bad = 0;
    for (const std::vector<double>& c : planes) {
      CHECK(c.size() == 32u * 32u);
      for (double v : c) {
        if (!std::isfinite(v)) ++bad;
        else sum += v, peak = std::max(peak, v);
      }
    }
    std::printf("shade_surface_ext grating card %s: display sum %.6g, peak %.6g, %u non-finite\n", device, sum, peak, bad);
    CHECK(bad == 0u);
    CHECK(peak > 0.0);
    std::filesystem::remove(fits);
  }
}

TEST_CASE("shade_surface_ext/agree cuda") {
  const Inputs in;
  const States st;
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr, in, st);
  const Snapshot g = run(*gpu, gpu.get(), in, st);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(c.term == g.term);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.flags == g.flags);
  CHECK(c.ray_type == g.ray_type);
  CHECK(c.event_index == g.event_index);
  uint32_t bounce_diff = 0, lambda_diff = 0, active = 0, terminated = 0, four = 0;
  double beta_rel = 0.0, dir = 0.0;
  for (uint32_t i = 0; i < kN; ++i) {
    for (int k = 0; k < 6; ++k) bounce_diff += c.bounces[i].v[k] != g.bounces[i].v[k] ? 1u : 0u;
    for (int l = 0; l < ar::kLanes; ++l) lambda_diff += c.lambda_t[i].v[l] != g.lambda_t[i].v[l] ? 1u : 0u;
    if (c.term[i] != uint8_t(ar::Term::ACTIVE)) continue;
    ++active;
    ((c.flags[i] & ar::SECONDARIES_TERMINATED) != 0u ? terminated : four) += 1u;
    for (int l = 0; l < ar::kLanes; ++l) {
      const double a = double(c.beta[i].v[l]), b = double(g.beta[i].v[l]);
      beta_rel = std::max(beta_rel, std::fabs(a - b) / std::max(std::fabs(a), 1e-30));
    }
    dir = std::max({dir, std::fabs(double(c.ray_d[i].x) - double(g.ray_d[i].x)), std::fabs(double(c.ray_d[i].y) - double(g.ray_d[i].y)),
                    std::fabs(double(c.ray_d[i].z) - double(g.ray_d[i].z))});
  }
  std::printf("shade_surface_ext agree: %u active (%u four lanes, %u terminated), beta max rel %.3g, direction max %.3g\n", active,
              four, terminated, beta_rel, dir);
  CHECK(bounce_diff == 0u);
  CHECK(lambda_diff == 0u);
  CHECK(four > kN / 8u);
  CHECK(terminated > kN / 32u);
  CHECK(beta_rel <= 1e-6);
  CHECK(dir <= 1e-6);
  CHECK(factor_mismatches(in, st, c) == 0u);
  CHECK(factor_mismatches(in, st, g) == 0u);
}

}  // TEST_SUITE
