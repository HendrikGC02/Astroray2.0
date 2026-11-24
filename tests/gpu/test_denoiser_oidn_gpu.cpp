// SPDX-License-Identifier: Apache-2.0
// The OIDN backend on a CUDA device created on the engine's stream (GPU renders): it lowers V11's
// relMSE on the noisy fixture in both modes, records its device memory under DENOISER as the drop
// in free device bytes and gives it back when destroyed (also while loading), fails loudly for a
// device it cannot use, and leaves the engine's CUDA device usable. sm_120 support is measured
// here (0008 Risks 3). The denoised images are written to out/check/denoiser_oidn/ for a look.
// Ref: docs/decisions/0008-film-outputs-denoise.md d16, d18, V11, Risks 3; 0007 d14 (ledger).
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/noisy_fixture.h"
#include "device/cuda/cuda_device.h"
#include "host/denoise/denoiser.h"

using namespace astroray;
using namespace astroray::host;
using astroray::test::NoisyFixture;

namespace {

constexpr Owner kOwner{1, 2};

DenoiserConfig cuda_config(CudaDevice& dev, Ledger& ledger) {
  DenoiserConfig c;
  c.cuda_device = dev.device_ordinal();
  c.cuda_stream = dev.native_stream();
  c.ledger = &ledger;
  c.owner = kOwner;
  return c;
}

DenoiseInputs fixture_inputs(const NoisyFixture& f) {
  DenoiseInputs in;
  in.color = {f.noisy.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  in.albedo = {f.albedo.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  in.normal = {f.normal.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  return in;
}

std::filesystem::path dump_dir() {
  const std::filesystem::path here = __FILE__;  // <root>/tests/gpu/test_denoiser_oidn_gpu.cpp
  return here.parent_path().parent_path().parent_path() / "out" / "check" / "denoiser_oidn";
}

// Raw little-endian float32, `stride` floats per pixel, for out/tmp/176's look script.
void dump(const std::string& name, const float* data, size_t floats) {
  std::filesystem::create_directories(dump_dir());
  std::ofstream(dump_dir() / (name + ".f32"), std::ios::binary)
      .write(reinterpret_cast<const char*>(data), std::streamsize(floats * sizeof(float)));
}

// Proves the engine's device still works: an upload and a download round trip.
void check_round_trip(CudaDevice& dev) {
  DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, 256, kOwner);
  std::vector<uint8_t> in(256), back(256, 0);
  for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 7 + 3);
  dev.upload(b, 0, in.data(), in.size());
  dev.download(back.data(), b, 0, back.size());
  dev.free(b);
  CHECK(back == in);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("denoiser_oidn_gpu/cuda lowers relmse") {
  ContextState ctx;
  Ledger ledger;
  CudaDevice dev(ledger, 0);
  auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, cuda_config(dev, ledger));
  state->wait();
  INFO(state->error());
  REQUIRE(state->ready());  // 0008 Risks 3: a failure here is the decision-table outcome, with OIDN's text
  std::printf("denoiser cuda: %s\n", state->description().c_str());
  CHECK(state->description().find("CUDA device 0") != std::string::npos);
  const NoisyFixture f = test::make_noisy_fixture();
  const double raw = test::rel_mse(f.noisy.data(), 3, f.clean);
  dump("clean", f.clean.data(), f.clean.size());
  dump("noisy", f.noisy.data(), f.noisy.size());
  // The CPU device on the same fixture, for the CPU/CUDA comparison of the look.
  auto cpu = DenoiserState::create(ctx, DenoiserBackend::OIDN);
  cpu->wait();
  INFO(cpu->error());
  REQUIRE(cpu->ready());
  for (DenoiseMode mode : {DenoiseMode::FINAL, DenoiseMode::VIEWPORT}) {
    const char* label = mode == DenoiseMode::FINAL ? "final" : "viewport";
    DisplayImage out, out_cpu;
    denoise(*state, fixture_inputs(f), out, mode);
    denoise(*cpu, fixture_inputs(f), out_cpu, mode);
    REQUIRE(out.width == NoisyFixture::kSize);
    REQUIRE(out.height == NoisyFixture::kSize);
    CHECK(out.denoised);
    const double den = test::rel_mse(out.rgba.data(), 4, f.clean);
    const double den_cpu = test::rel_mse(out_cpu.rgba.data(), 4, f.clean);
    std::printf("denoiser cuda %s: relMSE raw %.5f denoised %.5f ratio %.4f (cpu ratio %.4f)\n", label, raw, den,
                den / raw, den_cpu / raw);
    CHECK(den < raw);
    dump(std::string("cuda_") + label, out.rgba.data(), out.rgba.size());
    dump(std::string("cpu_") + label, out_cpu.rgba.data(), out_cpu.rgba.size());
  }
  state.reset();
  check_round_trip(dev);
}

TEST_CASE("denoiser_oidn_gpu/forced creation failure") {
  ContextState ctx;
  Ledger ledger;
  CudaDevice dev(ledger, 0);
  const float rgb[3] = {0.5f, 0.5f, 0.5f};
  DenoiseInputs in;
  in.color = {rgb, 1, 1, 3};
  struct Case {
    const char* what;
    const char* named;  // what the error must name
    DenoiserConfig config;
  };
  Case cases[2] = {{"CUDA ordinal 99", "CUDA device 99", cuda_config(dev, ledger)},
                   {"empty library_dir", "astroray_oidn.dll", cuda_config(dev, ledger)}};
  cases[0].config.cuda_device = 99;
  cases[1].config.library_dir.clear();
  for (const Case& c : cases) {
    INFO(c.what);
    auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, c.config);
    state->wait();
    REQUIRE(state->failed());
    const std::string e = state->error();
    std::printf("denoiser cuda forced failure (%s): %s\n", c.what, e.c_str());
    CHECK(e.find(c.named) != std::string::npos);
    DisplayImage out;
    CHECK_THROWS_WITH_AS(denoise(*state, in, out, DenoiseMode::FINAL), doctest::Contains(c.named), std::runtime_error);
    // The caller shows the noisy image with denoised = false (d16): `out` was not written.
    CHECK_FALSE(out.denoised);
    CHECK(out.rgba.empty());
    CHECK(ledger.category_total(LedgerCategory::DENOISER) == 0);
  }
  check_round_trip(dev);
}

TEST_CASE("denoiser_oidn_gpu/ledger holds the denoiser bytes") {
  ContextState ctx;
  Ledger ledger;
  CudaDevice dev(ledger, 0);
  {
    auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, cuda_config(dev, ledger));
    state->wait();
    INFO(state->error());
    REQUIRE(state->ready());
    // Creation commits and runs both modes once, so the device's memory is held already.
    const uint64_t created = ledger.category_total(LedgerCategory::DENOISER);
    CHECK(created > 0);
    CHECK(created == state->device_bytes());
    DisplayImage out;
    const NoisyFixture f = test::make_noisy_fixture();
    denoise(*state, fixture_inputs(f), out, DenoiseMode::FINAL);
    denoise(*state, fixture_inputs(f), out, DenoiseMode::VIEWPORT);
    const uint64_t total = ledger.category_total(LedgerCategory::DENOISER);
    std::printf("denoiser cuda ledger: %llu bytes after creation, %llu after FINAL and VIEWPORT commits\n",
                static_cast<unsigned long long>(created), static_cast<unsigned long long>(total));
    CHECK(total > 0);
    CHECK(total == state->device_bytes());
    for (const LedgerRecord& r : ledger.records())
      if (r.category == LedgerCategory::DENOISER) CHECK((r.owner.scene == kOwner.scene && r.owner.session == kOwner.session));
  }
  CHECK(ledger.category_total(LedgerCategory::DENOISER) == 0);
  // Destroyed at once, mostly while the library or the device is still loading.
  for (int i = 0; i < 20; ++i) {
    auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, cuda_config(dev, ledger));
    state.reset();
    REQUIRE(ledger.category_total(LedgerCategory::DENOISER) == 0);
  }
  check_round_trip(dev);
}

}  // TEST_SUITE
