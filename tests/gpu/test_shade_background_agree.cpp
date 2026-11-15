// SPDX-License-Identifier: Apache-2.0
// shade_background on the GPU against the CPU float stage (0009 d17 step iii, the per-stage
// fixture of 0002 D4): 10^5 synthetic camera-path misses over a table of [decoy lamp, world, decoy
// lamp], two samples per pixel so the path-end statistics are shared between paths, with random
// wavelengths, throughputs (1% non-finite), bounce counts and hero lanes. Integer fields (term,
// next_stage, the slot flags and every COUNT plane) must be bit-equal; the slot radiance agrees to
// 0002 D12's evaluation threshold (FMA-off GPU build vs CPU float).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D12; 0009 d17; 0004 D5, D28.
// The production module is launched, so what runs is the shipped code.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "agree.h"
#include "device/cpu/cpu_device.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/thread_pool.h"
#include "stages/shade_background.h"


using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kFilmW = 250, kFilmH = 200, kK = 2;
constexpr uint32_t kN = kFilmW * kFilmH * kK;  // 10^5 paths

// One device's copy of the fixture.
struct Snapshot {
  std::vector<float> slot_L;
  std::vector<uint32_t> slot_flags;
  std::vector<uint8_t> next_stage, term;
  std::vector<uint32_t> counts;  // every COUNT plane
  std::vector<float> adds;       // every ADD plane (the bounce sum: exact integers)
};

std::vector<float> observer_illuminant() {
  std::vector<float> t(ar::kObsSamples);
  for (int i = 0; i < ar::kObsSamples; ++i) t[size_t(i)] = 1.0f + 0.5f * std::sin(float(i) / 37.0f);
  return t;
}

Snapshot run(Device& dev, ModuleDevice* gpu) {
  const Owner owner{1, 1};
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    return b;
  };
  FilmRequest req;
  req.max_width = kFilmW;
  req.max_height = kFilmH;
  req.slot_capacity = kN;
  req.statistics = true;
  req.termination_stats = true;
  Film film(dev, FilmLayout::build(req), owner);
  DeviceBuffer film_buf = put(&film.data(), sizeof(ar::FilmData));

  std::vector<uint32_t> identity(kN), list(kFilmW * kFilmH);
  for (uint32_t i = 0; i < kN; ++i) identity[i] = i;
  for (uint32_t i = 0; i < list.size(); ++i) list[i] = i;
  DeviceBuffer queue_buf = put(identity.data(), kN * sizeof(uint32_t));
  DeviceBuffer list_buf = put(list.data(), list.size() * sizeof(uint32_t));
  const ar::StepEntry step{0, kN, 0};
  ar::StepTable table{};
  table.stage[uint32_t(ar::Stage::SHADE_BACKGROUND)] = step;
  DeviceBuffer table_buf = put(&table, sizeof table);

  // Path state, inputs from the sampler's independent stream (agree::unit), never a rand().
  std::vector<uint8_t> next(kN, uint8_t(ar::Stage::SHADE_BACKGROUND)), term(kN, uint8_t(ar::Term::ACTIVE));
  std::vector<uint32_t> flags(kN), sample_id(kN);
  std::vector<ar::U16x6> bounces(kN);
  std::vector<ar::Spec> lambda_t(kN), beta(kN);
  const float inf = std::numeric_limits<float>::infinity();
  for (uint32_t i = 0; i < kN; ++i) {
    for (int l = 0; l < ar::kLanes; ++l) {
      lambda_t[i].v[l] = 380.0f + 400.0f * float(agree::unit(i, uint32_t(l)));
      beta[i].v[l] = 2.0f * float(agree::unit(i, 4u + uint32_t(l)));
    }
    const double bad = agree::unit(i, 8);  // 1% of paths carry a non-finite throughput lane
    if (bad < 0.005) beta[i].v[int(agree::unit(i, 9) * 4.0)] = std::numeric_limits<float>::quiet_NaN();
    else if (bad < 0.01) beta[i].v[int(agree::unit(i, 9) * 4.0)] = inf;
    sample_id[i] = i;
    flags[i] = ar::set_hero_lane(uint32_t(agree::unit(i, 10) * 4096.0) << 9, uint32_t(agree::unit(i, 11) * 4.0) & 3u) &
               ~ar::PATH_KIND;
    bounces[i] = ar::U16x6{{uint16_t(agree::unit(i, 12) * 13.0), 0, 0, 0, 0, 0}};
  }
  DeviceBuffer next_buf = put(next.data(), kN), term_buf = put(term.data(), kN);
  DeviceBuffer flags_buf = put(flags.data(), kN * sizeof(uint32_t));
  DeviceBuffer bounces_buf = put(bounces.data(), kN * sizeof(ar::U16x6));
  DeviceBuffer lambda_buf = put(lambda_t.data(), kN * sizeof(ar::Spec));
  DeviceBuffer beta_buf = put(beta.data(), kN * sizeof(ar::Spec));
  ar::PathStateSoA soa{};
  DeviceBuffer sample_buf = put(sample_id.data(), kN * sizeof(uint32_t));
  soa.sample_id = static_cast<uint32_t*>(sample_buf.ptr);
  soa.next_stage = static_cast<uint8_t*>(next_buf.ptr);
  soa.term = static_cast<uint8_t*>(term_buf.ptr);
  soa.flags = static_cast<uint32_t*>(flags_buf.ptr);
  soa.bounces = static_cast<ar::U16x6*>(bounces_buf.ptr);
  soa.lambda_t = static_cast<ar::Spec*>(lambda_buf.ptr);
  soa.beta = static_cast<ar::Spec*>(beta_buf.ptr);
  DeviceBuffer soa_buf = put(&soa, sizeof soa);

  // The light table: an RGB illuminant world between two decoy lamps, so a wrong index would read a lamp.
  ar::SpectrumRef colour{};
  colour.kind = uint8_t(ar::SpectrumKind::RGB);
  colour.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  colour.p[0] = 0.3f, colour.p[1] = -0.2f, colour.p[2] = 0.1f, colour.p[3] = 0.9f;
  const ar::LightRecord decoy =
      ar::light_make(ar::LightType::POINT, ar::Real3{1, 2, 3}, ar::Real(0), ar::Real(50), colour);
  const std::vector<ar::LightRecord> lights{decoy, ar::light_make_world(ar::Real(1.75), colour), decoy};
  DeviceBuffer lights_buf = put(lights.data(), lights.size() * sizeof(ar::LightRecord));
  ar::LightSamplerView sampler{};
  sampler.light_count = uint32_t(lights.size());
  DeviceBuffer sampler_buf = put(&sampler, sizeof sampler);
  const std::vector<float> illum = observer_illuminant();
  const std::vector<float> cmf(3 * ar::kObsSamples, 0.0f);
  DeviceBuffer illum_buf = put(illum.data(), illum.size() * sizeof(float));
  DeviceBuffer cmf_buf = put(cmf.data(), cmf.size() * sizeof(float));
  ar::SceneTables tables{};
  tables.observer = ar::ObserverView{static_cast<const float*>(cmf_buf.ptr), static_cast<const float*>(illum_buf.ptr)};
  DeviceBuffer tables_buf = put(&tables, sizeof tables);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue_buf.ptr);
  kg.active_pixels = static_cast<uint32_t*>(list_buf.ptr);
  kg.film = static_cast<ar::FilmData*>(film_buf.ptr);
  kg.tables = static_cast<const ar::SceneTables*>(tables_buf.ptr);
  kg.lights = static_cast<const ar::LightRecord*>(lights_buf.ptr);
  kg.light_sampler = static_cast<const ar::LightSamplerView*>(sampler_buf.ptr);
  kg.world_light = 1;  // [decoy, world, decoy]
  kg.session.width = kFilmW;
  kg.session.height = kFilmH;
  kg.session.samples_per_pass = kK;
  DeviceBuffer kg_buf = put(&kg, sizeof kg);

  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_background.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_background", (kN + 255) / 256, 256, args);
  } else {
    // On the CPU device the uploaded copies are host memory the stage reads directly.
    const ar::KernelGlobals host_kg = kg;
    cpu_stage_shade_background(host_kg, step, 0, kN);
  }
  dev.synchronize();

  Snapshot s;
  s.slot_L.resize(size_t(kN) * 4);
  s.slot_flags.resize(kN);
  s.next_stage.resize(kN);
  s.term.resize(kN);
  const ar::FilmData& fd = film.data();
  const auto offset = [&](const void* p) { return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film.buffer().ptr)); };
  dev.download(s.slot_L.data(), film.buffer(), offset(fd.slot_L), s.slot_L.size() * sizeof(float));
  dev.download(s.slot_flags.data(), film.buffer(), offset(fd.slot_flags), kN * sizeof(uint32_t));
  dev.download(s.next_stage.data(), next_buf, 0, kN);
  dev.download(s.term.data(), term_buf, 0, kN);
  // The COUNT planes are the contiguous region from fd.count; the ADD plane of the bounce sum is exact.
  const uint32_t n_count_planes = uint32_t(fd.term_hist) + ar::kTermCount;
  s.counts.resize(size_t(n_count_planes) * fd.plane_stride);
  dev.download(s.counts.data(), film.buffer(), offset(fd.count), s.counts.size() * sizeof(uint32_t));
  s.adds.resize(size_t(uint32_t(fd.bounce_sum) + 1u) * fd.plane_stride);
  dev.download(s.adds.data(), film.buffer(), offset(fd.add), s.adds.size() * sizeof(float));

  for (DeviceBuffer* b : {&film_buf, &queue_buf, &list_buf, &table_buf, &next_buf, &term_buf, &flags_buf, &bounces_buf,
                          &lambda_buf, &beta_buf, &sample_buf, &soa_buf, &lights_buf, &sampler_buf, &illum_buf, &cmf_buf, &tables_buf,
                          &kg_buf})
    dev.free(*b);
  return s;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("shade_background/agreement") {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Snapshot c = run(cpu, nullptr);
  const Snapshot g = run(*gpu, gpu.get());
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);

  // Integer fields: bitwise.
  CHECK(c.term == g.term);
  CHECK(c.next_stage == g.next_stage);
  CHECK(c.slot_flags == g.slot_flags);
  CHECK(c.counts == g.counts);
  CHECK(c.adds == g.adds);
  // The fixture exercises both outcomes and every path ended.
  uint32_t escaped = 0, invalid = 0, ended = 0;
  for (uint32_t i = 0; i < kN; ++i) {
    escaped += c.term[i] == uint8_t(ar::Term::ESCAPED);
    invalid += c.term[i] == uint8_t(ar::Term::INVALID);
    ended += c.next_stage[i] == uint8_t(ar::Stage::TERMINATED);
  }
  CHECK(ended == kN);
  CHECK(invalid > 500u);
  CHECK(escaped + invalid == kN);
  // An invalid sample's slot is NaN on both devices (sample_mark_invalid): the NaN positions must match
  // exactly, and are zeroed before the finite comparison below.
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
  CHECK(total > 1.0e4);  // radiance really was added, so agreement is not two zeros

  // Radiance (4 lanes per path): the evaluation class.
  agree::Results rc, rg;
  rc.n = rg.n = kN;
  rc.stride = rg.stride = 4;
  rc.v.assign(lc.begin(), lc.end());
  rg.v.assign(lg.begin(), lg.end());
  const agree::Stats e = agree::compare(rg, rc, agree::Metric::REL_L2);
  CHECK(agree::report("shade_background", "slot radiance nofma vs cpu float", e, agree::Cls::EVAL,
                      agree::NOFMA_VS_CPU_FLOAT).pass);
}

}  // TEST_SUITE
