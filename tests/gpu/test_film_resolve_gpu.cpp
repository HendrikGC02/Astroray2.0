// SPDX-License-Identifier: Apache-2.0
// k_film_resolve on the GPU: the same moments as the CPU kernel for the same slots (agreement,
// 0008 V2), and the single-pixel statistics of tests/cpu/test_film_resolve.cpp on the device. The
// kernel module has no sample_add launcher (sample_add itself is covered by the CPU test), so
// the slot lanes are uploaded as sample_add would leave them in clean slots.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3, d4, d6, V2; 0002 D12 (FMA-off agreement
// build), D14.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/device.h"
#include "host/film/film_layout.h"
#include "host/film/response_table.h"
#include "host/spectral/wavelength_table.h"
#include "host/thread_pool.h"
#include "film/resolve.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// The tables of tests/cpu/test_film_resolve.cpp: flat gives x = v for a slot of v / 256 in every
// lane; varied has ramps, a two-bin wavelength table and a photon band at channel 3.
struct Tables {
  ResponseTable response;
  WavelengthTable wavelengths;
  explicit Tables(bool flat) {
    ResponseTable& t = response;
    t.display_first = 0;
    t.photon_scale = 1e-9 / (6.62607015e-34 * 299792458.0);
    t.y_w = flat ? 1.0 : 0.8;
    const double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double mv[9] = {1, 0.1, 0, 0, 1, 0.2, 0.1, 0, 1};
    for (int i = 0; i < 9; ++i) t.m_ws[i] = flat ? m[i] : mv[i];
    for (uint32_t c = 0; c < 4; ++c) {
      t.channels.push_back({2 * c, 2, (!flat && c == 3) ? ar::RESPONSE_PHOTON : ar::RESPONSE_ENERGY, 0});
      t.knot_nm.push_back(300.0f);
      t.knot_nm.push_back(900.0f);
      t.knot_value.push_back(1.0f);
      t.knot_value.push_back(flat ? 1.0f : 1.0f + 0.25f * float(c + 1));
    }
    if (flat) {
      wavelengths.edges = {360.0f, 616.0f};
      wavelengths.cdf = {0.0f, 1.0f};
    } else {
      wavelengths.edges = {360.0f, 500.0f, 700.0f};
      wavelengths.cdf = {0.0f, 0.25f, 1.0f};
    }
  }
};

struct SplitMix {
  uint64_t x;
  double next() {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return double((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);
  }
};

template <class T>
DeviceBuffer put(Device& dev, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, sizeof(T) * v.size(), Owner{1, 1});
  dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}

// A film on the CUDA device, its tables and KernelGlobals in device memory.
struct GpuRig {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev;
  uint32_t module;
  Tables tables;
  std::unique_ptr<Film> film;
  DeviceBuffer knot_nm, knot_value, channels, edges, cdf, film_data, kg_buf;
  ar::FilmData host_data;
  ar::KernelGlobals kg{};

  GpuRig(const FilmRequest& req, bool flat) : dev(make_cuda_device(ledger, 0)), tables(flat) {
    module = dev->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/film_resolve.cubin");
    film = std::make_unique<Film>(*dev, FilmLayout::build(req), Owner{1, 1});
    knot_nm = put(*dev, tables.response.knot_nm);
    knot_value = put(*dev, tables.response.knot_value);
    channels = put(*dev, tables.response.channels);
    edges = put(*dev, tables.wavelengths.edges);
    cdf = put(*dev, tables.wavelengths.cdf);
    host_data = film->data();
    ResponseTable view_source = tables.response;
    host_data.response = response_view(view_source);
    host_data.response.knot_nm = static_cast<const float*>(knot_nm.ptr);
    host_data.response.knot_value = static_cast<const float*>(knot_value.ptr);
    host_data.response.channels = static_cast<const ar::ResponseChannel*>(channels.ptr);
    host_data.wavelengths = {static_cast<const float*>(edges.ptr), static_cast<const float*>(cdf.ptr),
                             tables.wavelengths.bins()};
    film_data = put(*dev, std::vector<ar::FilmData>{host_data});
    kg.film = static_cast<ar::FilmData*>(film_data.ptr);
    kg.session.width = req.max_width;
    kg.session.height = req.max_height;
    kg.session.seed = 7u;
    kg_buf = put(*dev, std::vector<ar::KernelGlobals>{kg});
  }
  ~GpuRig() {
    dev->synchronize();
    for (DeviceBuffer* b : {&knot_nm, &knot_value, &channels, &edges, &cdf, &film_data, &kg_buf}) dev->free(*b);
    film.reset();
    CHECK(ledger.total() == 0);
  }

  uint64_t offset_of(const void* p) const {
    return uint64_t(static_cast<const char*>(p) - static_cast<const char*>(film->buffer().ptr));
  }
  // Slot lanes and flags as host data, uploaded into the film.
  void set_slots(const std::vector<float>& lanes, const std::vector<uint32_t>& flags) {
    dev->upload(film->buffer(), offset_of(host_data.slot_L), lanes.data(), lanes.size() * sizeof(float));
    dev->upload(film->buffer(), offset_of(host_data.slot_flags), flags.data(), flags.size() * sizeof(uint32_t));
  }
  void set_splat(const std::vector<float>& planes) {
    dev->upload(film->buffer(), offset_of(host_data.splat), planes.data(), planes.size() * sizeof(float));
  }
  // One pass: the session uniforms advance as the scheduler's do.
  void resolve(uint32_t k) {
    film->check_resolve_fits(k);
    kg.session.samples_per_pass = k;
    dev->upload(kg_buf, 0, &kg, sizeof(kg));
    const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
    void* args[] = {&kg_dev};
    const uint32_t count = host_data.rect_width * host_data.rect_height;
    dev->launch_module_kernel(module, "k_film_resolve", (count + 255) / 256, 256, args);
    kg.session.samples_done += k;
    kg.session.pass_index += 1;
  }
  template <class T>
  std::vector<T> read(const void* p, uint64_t count) {
    dev->synchronize();
    std::vector<T> out(count);
    dev->download(out.data(), film->buffer(), offset_of(p), count * sizeof(T));
    return out;
  }
};

std::vector<float> flat_lanes(uint32_t slots, float v) { return std::vector<float>(size_t(slots) * 4, v / 256.0f); }

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("film_resolve/gpu matches cpu") {
  FilmRequest req;
  req.max_width = 16, req.max_height = 8;
  req.slot_capacity = 16 * 8 * 3;
  req.adaptive_sampling = true;
  req.light_pass = true;
  req.extra = {{"band", ChannelKind::Moment, 1, 3}};
  const uint32_t pixels = 16 * 8, k = 3, slots = pixels * k;

  // CPU film on the CPU device with the same tables and data.
  Ledger cpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 1);
  CpuDevice cpu(cpu_ledger, pool);
  Tables tables(false);
  Film cf(cpu, FilmLayout::build(req), Owner{1, 1});
  ar::FilmData& cd = cf.data();
  cd.response = response_view(tables.response);
  cd.wavelengths = {tables.wavelengths.edges.data(), tables.wavelengths.cdf.data(), tables.wavelengths.bins()};
  ar::KernelGlobals ckg{};
  ckg.film = &cd;
  ckg.session.seed = 7u;

  GpuRig gpu(req, false);
  SplitMix rng{99};
  for (int pass = 0; pass < 3; ++pass) {
    std::vector<float> lanes(size_t(slots) * 4);
    std::vector<uint32_t> flags(slots);
    for (float& v : lanes) {
      const double r = rng.next();
      v = r < 0.03 ? kNaN : r < 0.06 ? -1.0f : float(rng.next() * 2.0);
    }
    for (uint32_t& f : flags) f = rng.next() < 0.25 ? ar::SECONDARIES_TERMINATED : 0u;
    std::vector<float> splat(size_t(3) * pixels);
    for (float& v : splat) v = pass == 1 ? float(rng.next()) : 0.0f;
    if (pass == 1) splat[5] = kNaN;  // a bad splat is dropped and counted on both devices

    gpu.set_slots(lanes, flags);
    gpu.set_splat(splat);
    gpu.resolve(k);

    std::memcpy(cd.slot_L, lanes.data(), lanes.size() * sizeof(float));
    std::memcpy(cd.slot_flags, flags.data(), flags.size() * sizeof(uint32_t));
    std::memcpy(cd.splat, splat.data(), splat.size() * sizeof(float));
    ckg.session.samples_per_pass = k;
    cpu_film_resolve(ckg, 0, pixels);
    ckg.session.samples_done += k;
    ckg.session.pass_index += 1;
  }

  const uint32_t planes = cf.layout().moment_planes();
  const uint64_t n_moment = uint64_t(planes) * pixels;
  const auto s1 = gpu.read<double>(gpu.host_data.s1, n_moment);
  const auto s2 = gpu.read<double>(gpu.host_data.s2, n_moment);
  const auto s1a = gpu.read<double>(gpu.host_data.s1a, n_moment);
  const auto n = gpu.read<uint32_t>(gpu.host_data.n, pixels);
  const auto invalid = gpu.read<uint32_t>(gpu.host_data.invalid, pixels);
  uint32_t differing = 0, total_invalid = 0;
  double worst = 0.0;
  const auto rel = [](double a, double b) { return a == b ? 0.0 : std::fabs(a - b) / std::max(std::fabs(a), std::fabs(b)); };
  for (uint32_t p = 0; p < pixels; ++p) {
    differing += n[p] != cd.n[p] || invalid[p] != cd.invalid[p];
    total_invalid += invalid[p];
  }
  for (uint64_t i = 0; i < n_moment; ++i)
    worst = std::max({worst, rel(s1[i], cd.s1[i]), rel(s2[i], cd.s2[i]), rel(s1a[i], cd.s1a[i])});
  MESSAGE("worst relative difference " << worst);
  CHECK(differing == 0);
  CHECK(total_invalid > 0);  // the fixture does inject bad samples
  CHECK(worst < 1e-6);
  // Slots and the splat plane are clean for the next pass.
  for (float v : gpu.read<float>(gpu.host_data.slot_L, uint64_t(slots) * 4)) CHECK(v == 0.0f);
  for (float v : gpu.read<float>(gpu.host_data.splat, uint64_t(3) * pixels)) CHECK(v == 0.0f);
}

TEST_CASE("film_resolve/gpu tail pass equals single sample passes and runs twice alike") {
  FilmRequest req;
  req.max_width = 2, req.max_height = 2;
  req.slot_capacity = 16;
  SplitMix rng{3};
  std::vector<float> table(4 * 4 * 4);  // [pixel][sample][lane]
  for (float& v : table) v = float(rng.next());
  const auto run = [&](bool tail) {
    GpuRig rig(req, false);
    if (tail) {
      rig.set_slots(table, std::vector<uint32_t>(16, 0u));
      rig.resolve(4);
    } else {
      for (uint32_t s = 0; s < 4; ++s) {
        std::vector<float> lanes(16 * 4, 0.0f);
        for (uint32_t j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i) lanes[(j * 1 + 0) * 4 + i] = table[(j * 4 + s) * 4 + i];
        rig.set_slots(lanes, std::vector<uint32_t>(16, 0u));
        rig.resolve(1);
      }
    }
    return rig.read<double>(rig.host_data.s1, 3 * 4);
  };
  const auto a = run(true), b = run(false), again = run(true);
  uint32_t differing = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    differing += a[i] != b[i];
    CHECK(std::fabs(a[i] - again[i]) <= 1e-6 * std::fabs(a[i]));
  }
  CHECK(differing == 0);
  CHECK(a[0] > 0.0);
}

TEST_CASE("film_resolve/gpu large sums exact") {
  // The CPU test streams 3e7 ones; one GPU thread resolves a 1x1 film at about 7 us per sample
  // (FP64 runs at 1/64 rate on this part), so the sum passes 2^24 here by a larger value
  // instead: 5e5 samples of 61 give S1 = 30,500,000, which float32 cannot hold exactly beyond
  // its 2^24 stall. The lanes are re-copied into the slots every pass, the resolve clears them.
  const uint32_t k = 2500, passes = 200;
  FilmRequest req;
  req.max_width = req.max_height = 1;
  req.slot_capacity = k;
  GpuRig rig(req, true);
  DeviceBuffer pristine = put(*rig.dev, flat_lanes(k, 61.0f));
  const uint64_t slot_offset = rig.offset_of(rig.host_data.slot_L);
  for (uint32_t p = 0; p < passes; ++p) {
    rig.dev->copy(rig.film->buffer(), slot_offset, pristine, 0, uint64_t(k) * 4 * sizeof(float));
    rig.resolve(k);
  }
  const auto s1 = rig.read<double>(rig.host_data.s1, 1);
  const auto s2 = rig.read<double>(rig.host_data.s2, 1);
  const auto n = rig.read<uint32_t>(rig.host_data.n, 1);
  rig.dev->free(pristine);
  CHECK(n[0] == 500000u);
  CHECK(s1[0] == 30500000.0);
  CHECK(s2[0] == 1860500000.0);
}

TEST_CASE("film_resolve/gpu variance of uniform samples") {
  const uint32_t n = 1u << 17, k = 4096;
  FilmRequest req;
  req.max_width = req.max_height = 1;
  req.slot_capacity = k;
  GpuRig rig(req, true);
  SplitMix rng{20260101};
  std::vector<float> xs(n);
  for (float& x : xs) x = float(rng.next());
  for (uint32_t p = 0; p < n / k; ++p) {
    std::vector<float> lanes(size_t(k) * 4);
    for (uint32_t m = 0; m < k; ++m)
      for (int i = 0; i < 4; ++i) lanes[m * 4 + i] = xs[p * k + m] / 256.0f;
    rig.set_slots(lanes, std::vector<uint32_t>(k, 0u));
    rig.resolve(k);
  }
  const double s1 = rig.read<double>(rig.host_data.s1, 1)[0];
  const double s2 = rig.read<double>(rig.host_data.s2, 1)[0];
  const double estimate = moment_variance_of_mean(s1, s2, n);
  double sum = 0.0;
  for (float x : xs) sum += double(x);
  const double m = sum / n;
  double ss = 0.0;
  for (float x : xs) ss += (double(x) - m) * (double(x) - m);
  CHECK(std::fabs(estimate - ss / (n - 1.0) / n) / estimate < 1e-6);
  // Uniform: sigma^2 = 1/12, kurtosis 1.8; the 99.9% interval of the sample variance.
  CHECK(std::fabs(estimate / (1.0 / 12.0 / n) - 1.0) < 3.29 * std::sqrt(0.8 / n));
}

}  // TEST_SUITE
