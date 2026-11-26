// SPDX-License-Identifier: Apache-2.0
// The tracking cases of 0006 V1 on CUDA: the chromatic absorbing slab against its spectral truth and the
// grey scattering slab against the independent random walk, with the specs and checks the CPU twins in
// tests/cpu/test_shade_volume.cpp use (common/slab_rig.h). Each device is checked against the truth, not
// against the other, so a fault shared by both targets still fails; the gate runs the two files in one
// concurrent group (CPU and GPU gate tests). The shadow case runs shade_shadow's ratio tracking on
// hand-built items against Beer-Lambert and against the CPU stage on the same items.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, D19, V1; 0002 D12.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/slab_rig.h"
#include "common/stat.h"
#include "device/cpu/cpu_device.h"
#include "device/cuda/cuda_device.h"
#include "device/cuda/optix_accel.h"
#include "host/thread_pool.h"
#include "stages/shade_shadow.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

namespace st = astroray::stat;

struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

SceneConfig cuda_config(const std::shared_ptr<Held>& h) {
  SceneConfig cfg;
  cfg.workers = 2;
  cfg.init = [h]() -> std::shared_ptr<void> {
    h->device = make_cuda_device(*h->ledger, 0);
    h->accel = make_optix_accel_builder(*h->device, 0, std::string());  // no OptiX disk cache
    return h;
  };
  cfg.device_of = [](void* p) -> Device* { return static_cast<Held*>(p)->device.get(); };
  cfg.accel_of = [](void* p) -> AccelBuilder* { return static_cast<Held*>(p)->accel.get(); };
  return cfg;
}

slab::Render render_cuda(slab::SlabRig& rig) {
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  slab::SlabSession session(rig, *device, Owner{0, 1});
  return session.converge();
}

// ---- shade_shadow's ratio tracking on hand-built items ----

constexpr uint32_t kShadowItems = 1u << 20, kShadowK = 64;  // 2^14 pixels x 64 samples

// The medium: chromatic RGB-sigmoid absorption 1.5 sigmoid(0.02 lambda - 11) per m, no scattering
// (the CPU twin's, tests/cpu/test_shade_shadow.cpp), through its volume program {reserved,
// VOLUME_COEFFS, END}.
ar::MediumBlock shadow_block() {
  ar::MediumBlock b = ar::medium_block_vacuum();
  b.sigma_a.kind = uint8_t(ar::SpectrumKind::RGB), b.sigma_a.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
  b.sigma_a.p[0] = 0.0f, b.sigma_a.p[1] = 0.02f, b.sigma_a.p[2] = -11.0f, b.sigma_a.p[3] = 1.5f;
  b.lobe_count = 0;
  b.entry = 1u;
  return b;
}

const ar::Spec kShadowLambda{{450, 520, 590, 660}};

// The stage over kShadowItems light-only items (contribution 1, r_u0 0, r_l0 1) in the medium, each
// over 1 m (ray_d of length 2, tmax 0.5), hero lanes cycling, medium_rng at event 3's start. Returns
// the per-sample buffer (4 lanes per item). The production module of the nofma build on CUDA, the
// CPU chunk function otherwise.
std::vector<float> run_shadow(Device& dev, ModuleDevice* gpu, uint32_t seed) {
  const Owner owner{1, 1};
  const uint32_t n = kShadowItems;
  std::vector<DeviceBuffer> bufs;
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, owner);
    dev.upload(b, 0, p, bytes);
    bufs.push_back(b);
    return b.ptr;
  };
  std::vector<uint32_t> queue(n), target(n), rng(n, ar::shadow_rng_start(3u)), active(n / kShadowK), slot_flags(n, 0u);
  std::vector<uint8_t> next(n, uint8_t(ar::Stage::SHADE_SHADOW)), iflags(n);
  std::vector<ar::Real3> o(n, ar::Real3{0, 0, 0}), d(n, ar::Real3{0, 0, 2});
  std::vector<ar::Real> tmax(n, ar::Real(0.5));
  std::vector<ar::Spec> contrib(n, ar::spec_fill(1)), lambda(n, kShadowLambda), r_u(n, ar::spec_fill(0)), r_l(n, ar::spec_fill(1));
  std::vector<ar::U32x4> stack(n, ar::shadow_medium_field(1u));
  std::vector<float> slot_L(4ull * n, 0.0f);
  for (uint32_t i = 0; i < n; ++i) queue[i] = target[i] = i, iflags[i] = uint8_t(ar::item_hero_bits(i & 3u));
  for (uint32_t j = 0; j < n / kShadowK; ++j) active[j] = j;
  const std::vector<uint32_t> program{0u, uint32_t(ar::Op::VOLUME_COEFFS), uint32_t(ar::Op::END)};
  const ar::MediumBlock block = shadow_block();
  const ar::MediumDesc desc{};
  ar::SceneData scene{};
  scene.medium_descs = static_cast<const ar::MediumDesc*>(put(&desc, sizeof desc)), scene.medium_count = 1;
  scene.medium_blocks = static_cast<const ar::MediumBlock*>(put(&block, sizeof block));
  scene.medium_programs = static_cast<const uint32_t*>(put(program.data(), program.size() * 4));
  scene.medium_program_words = uint32_t(program.size());
  const ar::SceneTables tables{};
  ar::ShadowItemSoA sh{};
  sh.next_stage = static_cast<uint8_t*>(put(next.data(), n));
  sh.ray_o = static_cast<ar::Real3*>(put(o.data(), n * sizeof(ar::Real3)));
  sh.ray_d = static_cast<ar::Real3*>(put(d.data(), n * sizeof(ar::Real3)));
  sh.tmax = static_cast<ar::Real*>(put(tmax.data(), n * sizeof(ar::Real)));
  sh.contribution = static_cast<ar::Spec*>(put(contrib.data(), n * sizeof(ar::Spec)));
  sh.target = static_cast<uint32_t*>(put(target.data(), n * 4));
  sh.item_flags = static_cast<uint8_t*>(put(iflags.data(), n));
  sh.lambda_t = static_cast<ar::Spec*>(put(lambda.data(), n * sizeof(ar::Spec)));
  sh.r_u = static_cast<ar::Spec*>(put(r_u.data(), n * sizeof(ar::Spec)));
  sh.r_l_film_pos = static_cast<ar::Spec*>(put(r_l.data(), n * sizeof(ar::Spec)));
  sh.medium_stack = static_cast<ar::U32x4*>(put(stack.data(), n * sizeof(ar::U32x4)));
  sh.medium_rng = static_cast<uint32_t*>(put(rng.data(), n * 4));
  ar::FilmData film{};
  float* slots = static_cast<float*>(put(slot_L.data(), slot_L.size() * 4));
  const DeviceBuffer slots_buf = bufs.back();
  film.slot_capacity = n, film.slot_L = slots;
  film.slot_flags = static_cast<uint32_t*>(put(slot_flags.data(), n * 4));
  film.bounce_sum = film.bounce_max = film.bounce_paths = film.term_hist = -1;
  film.guide_albedo = film.guide_normal = -1;
  ar::KernelGlobals kg{};
  kg.scene = static_cast<const ar::SceneData*>(put(&scene, sizeof scene));
  kg.tables = static_cast<const ar::SceneTables*>(put(&tables, sizeof tables));
  kg.shadow = static_cast<const ar::ShadowItemSoA*>(put(&sh, sizeof sh));
  kg.film = static_cast<ar::FilmData*>(put(&film, sizeof film));
  kg.shadow_queue = static_cast<const uint32_t*>(put(queue.data(), n * 4));
  kg.active_pixels = static_cast<uint32_t*>(put(active.data(), active.size() * 4));
  kg.session.samples_per_pass = kShadowK, kg.session.seed = seed, kg.session.sampler_mode = ar::SAMPLER_SOBOL;
  kg.session.features.media = true;
  ar::StepTable table{};
  const ar::StepEntry step{0, n, 0};
  table.stage[uint32_t(ar::Stage::SHADE_SHADOW)] = step;
  kg.step_table = static_cast<const ar::StepTable*>(put(&table, sizeof table));
  void* kg_p = put(&kg, sizeof kg);
  if (gpu != nullptr) {
    const uint32_t module = gpu->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/shade_shadow.cubin");
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_p);
    void* args[] = {&kg_dev};
    gpu->launch_module_kernel(module, "k_shade_shadow", (n + 255) / 256, 256, args);
  } else {
    cpu_stage_shade_shadow(kg, step, 0, n);
  }
  dev.synchronize();
  std::vector<float> out(4ull * n);
  dev.download(out.data(), slots_buf, 0, out.size() * 4);
  for (DeviceBuffer& b : bufs) dev.free(b);
  return out;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("volume_agree/chromatic slab cuda") {
  const auto held = std::make_shared<Held>();
  slab::SlabRig rig(cuda_config(held), slab::chromatic_spec());
  const slab::Render r = render_cuda(rig);
  slab::check_chromatic(rig, r, "volume_agree/chromatic slab cuda");
}

TEST_CASE("volume_agree/scattering slab cuda") {
  const auto held = std::make_shared<Held>();
  slab::SlabRig rig(cuda_config(held), slab::scattering_spec());
  const slab::Render r = render_cuda(rig);
  slab::check_scattering(r, "volume_agree/scattering slab cuda");
}

// shade_shadow's medium branch on CUDA (0006 D8, D19): 2^20 light-only items over 1 m of the chromatic
// medium, Sobol keys, hero lanes cycling. Per lane the CUDA mean against exp(-sigma d), one gate member
// per lane; then the CPU stage on the same items, every delivered value within 1e-6 relative (and the
// blocked items the same). The CPU twin is shade_shadow/transmittance per lane.
TEST_CASE("volume_agree/shadow transmittance cuda") {
  const st::Run run = st::begin("volume_agree/shadow transmittance cuda", st::gate_family());
  Ledger cpu_ledger, gpu_ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const std::vector<float> g = run_shadow(*gpu, gpu.get(), uint32_t(run.seed));
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  const std::vector<float> c = run_shadow(cpu, nullptr, uint32_t(run.seed));
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
  // The truth's coefficients from the volume entry on the host.
  const std::vector<uint32_t> program{0u, uint32_t(ar::Op::VOLUME_COEFFS), uint32_t(ar::Op::END)};
  const ar::MediumBlock block = shadow_block();
  ar::Lambda lam;
  for (int j = 0; j < ar::kLanes; ++j) lam.nm[j] = kShadowLambda.v[j];
  ar::MediumSample ms = ar::medium_sample_zero();
  ar::VmRegs regs;
  const ar::SceneTables no_tables{};
  REQUIRE(ar::medium_eval(ar::MediumTables{program.data(), &block, &no_tables}, ar::MediumDesc{}, ar::Real3{0, 0, 0}, lam, &regs,
                          &ms) == ar::VmStatus::OK);
  std::vector<double> samples[4];
  double worst = 0.0;
  uint32_t delivered = 0, mismatched = 0;
  for (uint32_t i = 0; i < kShadowItems; ++i) {
    delivered += g[4ull * i] != 0.0f;
    mismatched += (g[4ull * i] != 0.0f) != (c[4ull * i] != 0.0f);
    for (int j = 0; j < 4; ++j) {
      const double gv = g[4ull * i + uint32_t(j)], cv = c[4ull * i + uint32_t(j)];
      samples[j].push_back(gv);
      if (cv != 0.0) worst = std::max(worst, std::fabs(gv - cv) / std::fabs(cv));
    }
  }
  for (int j = 0; j < 4; ++j) {
    const double truth = std::exp(-double(ms.sigma_a.v[j] + ms.sigma_s.v[j]));
    const st::EnergyResult e = st::energy_test(run, samples[j], truth);
    std::printf("volume_agree shadow transmittance cuda lane %d: mean %.6f truth %.6f se %.2e z %+.2f\n", j, e.mean, truth, e.se, e.z);
    CHECK_MESSAGE(e.pass, e.message);
  }
  std::printf("volume_agree shadow transmittance: %u of %u delivered on CUDA, %u differ in outcome, worst relative %.3e\n",
              delivered, kShadowItems, mismatched, worst);
  CHECK(delivered > kShadowItems / 4);
  CHECK(mismatched == 0u);
  CHECK(worst <= 1e-6);
}

}  // TEST_SUITE
