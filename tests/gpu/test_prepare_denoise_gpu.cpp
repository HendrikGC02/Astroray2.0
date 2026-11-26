// SPDX-License-Identifier: Apache-2.0
// k_film_prepare_denoise on the GPU against the CPU kernel on the same random 64 x 64 film (the
// FMA-off agreement build): within 1e-6 relative everywhere, bit-identical where the value is a
// clamp (colour 0, albedo 0 or 1) or a pixel has no samples.
// Ref: docs/decisions/0008-film-outputs-denoise.md d15; 0002 D12 (FMA-off agreement build), D14.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/prepare_denoise.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

struct SplitMix {
  uint64_t x;
  uint64_t next() {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
};

constexpr uint32_t kSize = 64;

struct Rig {
  std::unique_ptr<Film> film;
  explicit Rig(Device& dev) {
    FilmRequest req;
    req.max_width = kSize;
    req.max_height = kSize;
    req.guides = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
  }
  uint64_t offset_of(const void* p) const {
    return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film->buffer().ptr));
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("prepare_denoise_gpu/cuda agrees with cpu") {
  Ledger cpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  Rig cr(cpu);

  Ledger ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(ledger, 0);
  const std::string dir = AR_TEST_KERNEL_NOFMA_DIR;
  const uint32_t module = gpu->load_module_file(dir + "/film_prepare_denoise.cubin");
  Rig gr(*gpu);

  // Random planes: signed display moments (negative means must clamp), counts including 0, guide
  // sums that reach below 0 and above n (albedo clamps) and normals of either sign.
  ar::FilmData& cd = cr.film->data();
  const ar::FilmData gd = gr.film->data();
  const uint32_t stride = cd.plane_stride, pixels = kSize * kSize;
  SplitMix rng{20261127};
  std::vector<double> s1(size_t(cr.film->layout().moment_planes()) * stride);
  std::vector<uint32_t> n(pixels);
  std::vector<float> add(size_t(cr.film->layout().add_planes()) * stride);
  for (double& v : s1) v = (rng.uniform() - 0.35) * 400.0;
  for (uint32_t& v : n) v = rng.uniform() < 0.1 ? 0u : uint32_t(1 + rng.next() % 64);
  for (float& v : add) v = float((rng.uniform() - 0.25) * 100.0);
  std::memcpy(cd.s1, s1.data(), s1.size() * sizeof(double));
  std::memcpy(cd.n, n.data(), n.size() * sizeof(uint32_t));
  std::memcpy(cd.add, add.data(), add.size() * sizeof(float));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.s1), s1.data(), s1.size() * sizeof(double));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.n), n.data(), n.size() * sizeof(uint32_t));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.add), add.data(), add.size() * sizeof(float));

  const uint32_t count = pixels;
  std::vector<uint32_t> list(count);
  for (uint32_t j = 0; j < count; ++j) list[j] = ar::film_active_pixel(cd, j);
  DeviceBuffer list_buf = gpu->allocate(LedgerCategory::TABLES, list.size() * sizeof(uint32_t), Owner{1, 1});
  gpu->upload(list_buf, 0, list.data(), list.size() * sizeof(uint32_t));

  const size_t floats = size_t(count) * 3;
  DeviceBuffer out[3];
  for (DeviceBuffer& b : out) b = gpu->allocate(LedgerCategory::TABLES, floats * sizeof(float), Owner{1, 1});
  const ar::DenoiseTarget gt{static_cast<float*>(out[0].ptr), static_cast<float*>(out[1].ptr),
                             static_cast<float*>(out[2].ptr), kSize, kSize};
  DeviceBuffer target_buf = gpu->allocate(LedgerCategory::TABLES, sizeof(gt), Owner{1, 1});
  gpu->upload(target_buf, 0, &gt, sizeof(gt));

  DeviceBuffer gd_buf = gpu->allocate(LedgerCategory::TABLES, sizeof(ar::FilmData), Owner{1, 1});
  gpu->upload(gd_buf, 0, &gd, sizeof(gd));
  ar::KernelGlobals gkg{};
  gkg.film = static_cast<ar::FilmData*>(gd_buf.ptr);
  gkg.active_pixels = static_cast<uint32_t*>(list_buf.ptr);
  gkg.denoise = static_cast<ar::DenoiseTarget*>(target_buf.ptr);
  DeviceBuffer kg_buf = gpu->allocate(LedgerCategory::TABLES, sizeof(gkg), Owner{1, 1});
  gpu->upload(kg_buf, 0, &gkg, sizeof(gkg));
  const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
  void* args[] = {&kg_dev};
  gpu->launch_module_kernel(module, "k_film_prepare_denoise", (count + 255) / 256, 256, args);
  gpu->synchronize();

  std::vector<float> want[3], got[3];
  for (int i = 0; i < 3; ++i) want[i].assign(floats, -42.0f), got[i].assign(floats, -42.0f);
  ar::KernelGlobals ckg{};
  ckg.film = &cd;
  ckg.active_pixels = list.data();
  const ar::DenoiseTarget ct{want[0].data(), want[1].data(), want[2].data(), kSize, kSize};
  ckg.denoise = const_cast<ar::DenoiseTarget*>(&ct);
  cpu_film_prepare_denoise(ckg, 0, count);
  for (int i = 0; i < 3; ++i) gpu->download(got[i].data(), out[i], 0, floats * sizeof(float));

  uint32_t bad = 0, exact_checked = 0, zero_pixels = 0, clamped_colour = 0, clamped_albedo = 0;
  for (uint32_t j = 0; j < count; ++j) {
    const bool no_samples = n[list[j]] == 0u;
    zero_pixels += no_samples;
    for (uint32_t c = 0; c < 3; ++c) {
      const size_t k = size_t(j) * 3 + c;
      const bool colour_clamp = want[0][k] == 0.0f, albedo_clamp = want[1][k] == 0.0f || want[1][k] == 1.0f;
      clamped_colour += colour_clamp && !no_samples;
      clamped_albedo += albedo_clamp && !no_samples;
      for (int i = 0; i < 3; ++i) {
        const bool exact = no_samples || (i == 0 && colour_clamp) || (i == 1 && albedo_clamp);
        if (exact) {
          ++exact_checked;
          bad += std::memcmp(&got[i][k], &want[i][k], sizeof(float)) != 0;
        } else {
          bad += !(std::fabs(got[i][k] - want[i][k]) <= 1e-6f * std::fabs(want[i][k]));
        }
      }
    }
  }
  CHECK(bad == 0u);
  // The fixture reaches the no-samples and both clamp cases it claims to compare bitwise.
  CHECK(zero_pixels > 0u);
  CHECK(clamped_colour > 0u);
  CHECK(clamped_albedo > 0u);
  CHECK(exact_checked > 0u);

  for (DeviceBuffer& b : out) gpu->free(b);
  gpu->free(target_buf);
  gpu->free(gd_buf);
  gpu->free(kg_buf);
  gpu->free(list_buf);
}

}  // TEST_SUITE
