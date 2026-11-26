// SPDX-License-Identifier: Apache-2.0
// The final denoise in a CUDA session (OIDN's CUDA device on the session's stream): Combined is the
// denoised image with DENOISED set, closer to the 1024-spp CPU reference than Noisy Image (V11's
// criterion at test size); the denoiser's device memory is in the session's ledger under DENOISER while
// the session lives and gone after; a denoiser created on a CUDA ordinal that does not exist leaves the
// noisy image, one DENOISER_UNAVAILABLE entry with OIDN's text and the state CONVERGED. The CPU twin is
// tests/cpu/test_session_denoise.cpp; the scene and the reference are common/cornell_rig.h.
// Ref: docs/decisions/0008-film-outputs-denoise.md d13a, d16, d18, V11; 0007 d14 (ledger).
#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/cornell_rig.h"
#include "device/cuda/cuda_device.h"
#include "device/cuda/optix_accel.h"

using namespace astroray;
using namespace astroray::host;

namespace {

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
  cfg.device_ledger = h->ledger;
  return cfg;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("session_denoise_gpu/denoised lowers relmse") {
  const std::vector<float> ref = cornell::reference();
  const auto held = std::make_shared<Held>();
  cornell::CornellRig rig(cuda_config(held), cornell::kSpp);
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  {
    Session s(rig.scene, *device, cornell::options(AR_DENOISER_OIDN));
    s.set_view(cornell::view());
    s.start();
    cornell::settle(s, rig);
    REQUIRE(s.denoised());
    const std::vector<float> combined = cornell::read_mean(s, "Combined");
    const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
    CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) != 0);
    cornell::check_reference(ref, noisy, cornell::read_mean(s, "Noisy Image", AR_QUANTITY_VARIANCE), "cuda");
    const double rel_noisy = cornell::rel_mse(noisy, ref), rel_denoised = cornell::rel_mse(combined, ref);
    const SessionStats st = s.stats();
    std::printf("session_denoise cuda: relMSE noisy %.5f denoised %.5f (ratio %.4f); denoise %.1f ms, prepare and copy %.1f ms\n",
                rel_noisy, rel_denoised, rel_denoised / rel_noisy, st.denoise_ms, st.denoise_copy_ms);
    CHECK(rel_denoised < rel_noisy);
    cornell::dump("cuda_noisy", noisy);
    cornell::dump("cuda_denoised", combined);
    CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());
    // d16: OIDN's CUDA memory (creation and configuration records) and the target planes.
    const uint64_t held_bytes = ledger.category_total(LedgerCategory::DENOISER);
    std::printf("session_denoise cuda: DENOISER ledger %llu bytes while the session lives\n",
                static_cast<unsigned long long>(held_bytes));
    CHECK(held_bytes > 0);
  }
  CHECK(ledger.category_total(LedgerCategory::DENOISER) == 0);
}

TEST_CASE("session_denoise_gpu/failure leaves noisy and one entry") {
  const auto held = std::make_shared<Held>();
  cornell::CornellRig rig(cuda_config(held), cornell::kSpp);
  Ledger ledger;
  auto device = make_cuda_device(ledger, 0);
  cornell::EnvGuard hook("ASTRORAY_TEST_DENOISER_FAIL", "device");
  Session s(rig.scene, *device, cornell::options(AR_DENOISER_OIDN));
  REQUIRE(std::find(s.active_test_hooks().begin(), s.active_test_hooks().end(), "DENOISER_FAIL=device") !=
          s.active_test_hooks().end());
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  CHECK(s.progress().state == AR_STATE_CONVERGED);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  const std::string error = s.denoiser_error();
  std::printf("session_denoise cuda forced failure: %s\n", error.c_str());
  CHECK(error.find("CUDA device 99") != std::string::npos);
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE);
  REQUIRE(e.size() == 1);
  CHECK(e[0].detail == error.substr(0, 127));
  // A failed state holds no device memory, and the session allocates its target only for a denoiser
  // that works.
  CHECK(ledger.category_total(LedgerCategory::DENOISER) == 0);
}

}  // TEST_SUITE
