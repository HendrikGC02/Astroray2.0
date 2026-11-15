// SPDX-License-Identifier: Apache-2.0
// k_film_present and k_film_heatmap on the GPU: output bit-identical to the CPU kernels for the
// same moments and statistics planes (the half conversion and the viridis lookup are one source,
// film/present.h, compiled for both devices).
// Ref: docs/decisions/0007-blender-integration.md d29; 0008 d12; 0002 D12 (FMA-off agreement
// build), D14.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "film/present.h"
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

constexpr uint32_t kW = 16, kH = 8, kRectW = 13, kRectH = 7;

// A film on a device with the render sub-rectangle set.
struct Rig {
  Device& dev;
  std::unique_ptr<Film> film;
  explicit Rig(Device& d) : dev(d) {
    FilmRequest req;
    req.max_width = kW;
    req.max_height = kH;
    req.statistics = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->set_render_size(kRectW, kRectH);
  }
  uint64_t offset_of(const void* p) const {
    return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film->buffer().ptr));
  }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("present/gpu matches cpu") {
  Ledger cpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  Rig cr(cpu);

  Ledger ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(ledger, 0);
  const std::string dir = AR_TEST_KERNEL_NOFMA_DIR;
  const uint32_t present_module = gpu->load_module_file(dir + "/film_present.cubin");
  const uint32_t heatmap_module = gpu->load_module_file(dir + "/film_heatmap.cubin");
  Rig gr(*gpu);

  // The same random planes on both films: display means that span the half range (negative, tiny,
  // and beyond 65504), sample counts including 0, bounce sums and path counts including 0.
  ar::FilmData& cd = cr.film->data();
  const ar::FilmData gd = gr.film->data();
  const uint32_t stride = cd.plane_stride, pixels = kW * kH;
  SplitMix rng{20261115};
  std::vector<double> s1(size_t(cr.film->layout().moment_planes()) * stride);
  std::vector<uint32_t> n(pixels), paths(size_t(cr.film->layout().count_planes()) * stride);
  std::vector<float> add(size_t(cr.film->layout().add_planes()) * stride);
  for (double& v : s1) {
    const double mag = std::exp(std::log(1e-7) + rng.uniform() * (std::log(5e5) - std::log(1e-7)));
    v = (rng.next() & 1u) ? mag : -mag;
  }
  for (uint32_t& v : n) v = rng.uniform() < 0.1 ? 0u : uint32_t(1 + rng.next() % 64);
  for (uint32_t& v : paths) v = rng.uniform() < 0.1 ? 0u : uint32_t(rng.next() % 40);
  for (float& v : add) v = float(rng.uniform() * 200.0);
  std::memcpy(cd.s1, s1.data(), s1.size() * sizeof(double));
  std::memcpy(cd.n, n.data(), n.size() * sizeof(uint32_t));
  std::memcpy(cd.count, paths.data(), paths.size() * sizeof(uint32_t));
  std::memcpy(cd.add, add.data(), add.size() * sizeof(float));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.s1), s1.data(), s1.size() * sizeof(double));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.n), n.data(), n.size() * sizeof(uint32_t));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.count), paths.data(), paths.size() * sizeof(uint32_t));
  gpu->upload(gr.film->buffer(), gr.offset_of(gd.add), add.data(), add.size() * sizeof(float));

  DeviceBuffer gd_buf = gpu->allocate(LedgerCategory::TABLES, sizeof(ar::FilmData), Owner{1, 1});
  gpu->upload(gd_buf, 0, &gd, sizeof(gd));
  ar::KernelGlobals gkg{};
  gkg.film = static_cast<ar::FilmData*>(gd_buf.ptr);
  DeviceBuffer kg_buf = gpu->allocate(LedgerCategory::TABLES, sizeof(gkg), Owner{1, 1});
  gpu->upload(kg_buf, 0, &gkg, sizeof(gkg));
  ar::KernelGlobals ckg{};
  ckg.film = &cd;

  const uint32_t count = kRectW * kRectH;
  const uint32_t blocks = (count + 255) / 256;
  const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);

  // present
  {
    DeviceBuffer out = gpu->allocate(LedgerCategory::TABLES, count * 4 * sizeof(uint16_t), Owner{1, 1});
    ar::PresentParams gp{static_cast<uint16_t*>(out.ptr)};
    DeviceBuffer pbuf = gpu->allocate(LedgerCategory::TABLES, sizeof(gp), Owner{1, 1});
    gpu->upload(pbuf, 0, &gp, sizeof(gp));
    const ar::PresentParams* pdev = static_cast<const ar::PresentParams*>(pbuf.ptr);
    void* args[] = {&kg_dev, &pdev};
    gpu->launch_module_kernel(present_module, "k_film_present", blocks, 256, args);
    gpu->synchronize();
    std::vector<uint16_t> got(count * 4), want(count * 4);
    gpu->download(got.data(), out, 0, got.size() * sizeof(uint16_t));
    ar::PresentParams cp{want.data()};
    cpu_film_present(ckg, cp, 0, count);
    uint32_t differing = 0, clamped = 0;
    for (size_t i = 0; i < got.size(); ++i) {
      differing += got[i] != want[i];
      clamped += (want[i] & 0x7fffu) == 0x7bffu;
    }
    CHECK(differing == 0u);
    CHECK(clamped > 0u);  // the fixture does reach the clamp
    gpu->free(out);
    gpu->free(pbuf);
  }

  // heatmap, both modes
  for (uint32_t mode : {uint32_t(ar::HEATMAP_SAMPLE_COUNT), uint32_t(ar::HEATMAP_BOUNCE_COUNT)}) {
    DeviceBuffer out = gpu->allocate(LedgerCategory::TABLES, count * sizeof(uint32_t), Owner{1, 1});
    ar::HeatmapParams gp{static_cast<uint32_t*>(out.ptr), mode, mode == 0u ? 48.0f : 6.0f};
    DeviceBuffer pbuf = gpu->allocate(LedgerCategory::TABLES, sizeof(gp), Owner{1, 1});
    gpu->upload(pbuf, 0, &gp, sizeof(gp));
    const ar::HeatmapParams* pdev = static_cast<const ar::HeatmapParams*>(pbuf.ptr);
    void* args[] = {&kg_dev, &pdev};
    gpu->launch_module_kernel(heatmap_module, "k_film_heatmap", blocks, 256, args);
    gpu->synchronize();
    std::vector<uint32_t> got(count), want(count);
    gpu->download(got.data(), out, 0, got.size() * sizeof(uint32_t));
    ar::HeatmapParams cp{want.data(), mode, gp.range_max};
    cpu_film_heatmap(ckg, cp, 0, count);
    uint32_t differing = 0, grey = 0, coloured = 0;
    for (uint32_t i = 0; i < count; ++i) {
      differing += got[i] != want[i];
      grey += want[i] == ar::kHeatmapNoData;
      coloured += want[i] != ar::kHeatmapNoData;
    }
    CHECK(differing == 0u);
    CHECK(grey > 0u);
    CHECK(coloured > 0u);
    gpu->free(out);
    gpu->free(pbuf);
  }

  gpu->free(gd_buf);
  gpu->free(kg_buf);
  gr.film.reset();
  CHECK(ledger.total() == 0);
}

}  // TEST_SUITE
