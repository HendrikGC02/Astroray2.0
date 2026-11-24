// SPDX-License-Identifier: Apache-2.0
// The OIDN backend on its CPU device (CPU renders): it loads astroray_oidn.dll on the worker
// thread, lowers V11's relMSE on the noisy fixture in both modes, takes no device memory, fails
// loudly with the library named, and survives being destroyed while it loads.
// Ref: docs/decisions/0008-film-outputs-denoise.md d16, d18, V11; 0001 D9.1.
#include <doctest.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/noisy_fixture.h"
#include "host/denoise/denoiser.h"

using namespace astroray;
using namespace astroray::host;
using astroray::test::NoisyFixture;

namespace {

DenoiseInputs fixture_inputs(const NoisyFixture& f) {
  DenoiseInputs in;
  in.color = {f.noisy.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  in.albedo = {f.albedo.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  in.normal = {f.normal.data(), NoisyFixture::kSize, NoisyFixture::kSize, 3};
  return in;
}

// Denoises the fixture in `mode` and checks the result is strictly closer to the clean image.
void check_lowers_relmse(DenoiserState& state, const NoisyFixture& f, DenoiseMode mode, const char* label) {
  DisplayImage out;
  denoise(state, fixture_inputs(f), out, mode);
  REQUIRE(out.width == NoisyFixture::kSize);
  REQUIRE(out.height == NoisyFixture::kSize);
  CHECK(out.denoised);
  const double raw = test::rel_mse(f.noisy.data(), 3, f.clean);
  const double den = test::rel_mse(out.rgba.data(), 4, f.clean);
  std::printf("denoiser cpu %s: relMSE raw %.5f denoised %.5f ratio %.4f\n", label, raw, den, den / raw);
  CHECK(den < raw);
  for (size_t i = 3; i < out.rgba.size(); i += 4) REQUIRE(out.rgba[i] == 1.0f);  // alpha copied, 3-component colour
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("denoiser/oidn cpu lowers relmse") {
  ContextState ctx;
  Ledger ledger;
  DenoiserConfig config;
  config.ledger = &ledger;
  auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, config);
  state->wait();
  INFO(state->error());
  REQUIRE(state->ready());
  std::printf("denoiser cpu: %s\n", state->description().c_str());
  CHECK(state->description().find("CPU") != std::string::npos);
  const NoisyFixture f = test::make_noisy_fixture();
  check_lowers_relmse(*state, f, DenoiseMode::FINAL, "FINAL");
  check_lowers_relmse(*state, f, DenoiseMode::VIEWPORT, "VIEWPORT");
  // The CPU device takes no device memory, so nothing is recorded.
  CHECK(state->device_bytes() == 0);
  CHECK(ledger.total() == 0);
}

TEST_CASE("denoiser/oidn failure is loud") {
  ContextState ctx;
  const float rgb[3] = {0.5f, 0.5f, 0.5f};
  DenoiseInputs in;
  in.color = {rgb, 1, 1, 3};
  // An empty folder, and an absolute folder without the library: both name the library.
  for (const std::string dir : {std::string(), oidn_dev_library_dir() + "/no-such-folder"}) {
    INFO("library_dir '" << dir << "'");
    DenoiserConfig config;
    config.library_dir = dir;
    auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, config);
    state->wait();
    REQUIRE(state->failed());
    CHECK_FALSE(state->ready());
    const std::string e = state->error();
    std::printf("denoiser cpu forced failure: %s\n", e.c_str());
    CHECK(e.find("astroray_oidn.dll") != std::string::npos);
    if (!dir.empty()) CHECK(e.find("no-such-folder") != std::string::npos);
    DisplayImage out;
    CHECK_THROWS_AS(denoise(*state, in, out, DenoiseMode::FINAL), std::runtime_error);
    CHECK_THROWS_WITH_AS(denoise(*state, in, out, DenoiseMode::VIEWPORT), doctest::Contains("astroray_oidn.dll"),
                         std::runtime_error);
    // The caller shows the noisy image: nothing was written, and denoised stays false.
    CHECK_FALSE(out.denoised);
    CHECK(out.rgba.empty());
  }
}

TEST_CASE("denoiser/oidn destroy while loading") {
  ContextState ctx;
  Ledger ledger;
  DenoiserConfig config;
  config.ledger = &ledger;
  // Each state is destroyed at once, mostly while the library or the device is still loading.
  for (int i = 0; i < 20; ++i) {
    auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, config);
    state.reset();
  }
  CHECK(ledger.total() == 0);
  // Loading and unloading the library twenty times leaves it usable.
  auto state = DenoiserState::create(ctx, DenoiserBackend::OIDN, config);
  state->wait();
  INFO(state->error());
  REQUIRE(state->ready());
  check_lowers_relmse(*state, test::make_noisy_fixture(), DenoiseMode::VIEWPORT, "VIEWPORT after 20 destroys");
}

}  // TEST_SUITE
