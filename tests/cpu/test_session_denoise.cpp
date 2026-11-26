// SPDX-License-Identifier: Apache-2.0
// The final denoise in a CPU session (OIDN's CPU device): after convergence the session denoises once
// and only then reports CONVERGED; Combined is the denoised image with DENOISED set, Noisy Image is
// the film's and equals bit for bit the Combined of the same render without a denoiser, and the
// denoised image is closer to a 4096-spp reference than the noisy one (V11's criterion at test size).
// A denoiser that cannot be created leaves the noisy image, one DENOISER_UNAVAILABLE entry with the
// backend's text and the state CONVERGED; a cancel while the render waits for the denoiser returns
// within B-21 and leaves the noisy image with no entry; sessions destroyed while the denoiser loads
// free everything. The scene is common/cornell_rig.h. The C ABI path is
// tests/abi/test_session_passes.py; CUDA is tests/gpu/test_session_denoise_gpu.cpp.
// Ref: docs/decisions/0008-film-outputs-denoise.md d10, d13a, d16 (ruling 87's order), d17, V9, V11;
// 0007 d27; 0009 B-21.
#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/cornell_rig.h"
#include "host/film/fits_writer.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr unsigned kWorkers = 2;  // hosted ci has 2-4 cores
constexpr double kB21Ms = 5.0;    // 0009 B-21

bool has(const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); }

// Combined of a CPU render of the box without a denoiser.
std::vector<float> render_without_denoiser() {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_NONE));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  const std::vector<std::string> names = cornell::pass_names(s);
  for (const char* p : {"Noisy Image", "Denoising Albedo", "Denoising Normal"}) CHECK_FALSE(has(names, p));
  CHECK_FALSE(s.denoised());
  return cornell::read_mean(s, "Combined");
}

// The value of the FITS card `key` in `path`'s primary header ("" when absent).
std::string fits_card(const std::filesystem::path& path, const char* key) {
  std::ifstream in(path, std::ios::binary);
  std::string block(2880, '\0');
  while (in.read(block.data(), std::streamsize(block.size()))) {
    for (size_t at = 0; at < block.size(); at += 80) {
      const std::string card = block.substr(at, 80);
      if (card.compare(0, 8, std::string(key) + std::string(8 - std::strlen(key), ' ')) == 0) {
        const size_t v = card.find_first_not_of(' ', 10);
        return v == std::string::npos ? "" : card.substr(v, 1);
      }
      if (card.compare(0, 3, "END") == 0) return "";
    }
  }
  return "";
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("session_denoise/noisy image is the film") {
  const std::vector<float> without = render_without_denoiser();
  const std::vector<float> ref = cornell::reference();

  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  {
    Session s(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
    s.set_view(cornell::view());
    s.start();
    cornell::settle(s, rig);
    const std::vector<std::string> names = cornell::pass_names(s);
    for (const char* p : {"Combined", "Denoising Albedo", "Denoising Normal", "Noisy Image"}) CHECK(has(names, p));
    REQUIRE(s.denoised());
    const std::vector<float> combined = cornell::read_mean(s, "Combined");
    const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
    REQUIRE(noisy.size() == without.size());
    // d13a, d17: Noisy Image is the film's, so it is the render a session without a denoiser makes.
    CHECK(std::memcmp(noisy.data(), without.data(), noisy.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) != 0);
    const double rel_noisy = cornell::rel_mse(noisy, ref), rel_denoised = cornell::rel_mse(combined, ref);
    const SessionStats st = s.stats();
    std::printf("session_denoise cpu: relMSE noisy %.5f denoised %.5f (ratio %.4f); denoise %.1f ms, prepare and copy %.1f ms\n",
                rel_noisy, rel_denoised, rel_denoised / rel_noisy, st.denoise_ms, st.denoise_copy_ms);
    CHECK(rel_denoised < rel_noisy);
    CHECK(st.denoises == 1);
    // A denoised image has no variance: the film's is Noisy Image's.
    CHECK_THROWS_WITH_AS(cornell::read_mean(s, "Combined", AR_QUANTITY_VARIANCE), doctest::Contains("Noisy Image"),
                         std::invalid_argument);
    CHECK(cornell::read_mean(s, "Noisy Image", AR_QUANTITY_VARIANCE).size() == noisy.size());
    // The guides read raw: the albedo of the lit walls is above 0 somewhere.
    const std::vector<float> albedo = cornell::read_mean(s, "Denoising Albedo");
    CHECK(*std::max_element(albedo.begin(), albedo.end()) > 0.5f);
    CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());
    // The target planes are in the ledger under DENOISER while the session lives (OIDN's CPU device
    // records none of its own, 176).
    CHECK(dev->ledger().category_total(LedgerCategory::DENOISER) > 0);
    // V9: the film dump of a denoised session still says DENOISED = F (a session film has no band
    // channel yet, so the dump is the session's whole film).
    const std::filesystem::path fits = cornell::repo_root() / "out" / "tmp" / "197" / "session_denoise_cpu.fits";
    std::filesystem::create_directories(fits.parent_path());
    s.read_film([&](const FilmReadContext& c) { write_fits_film_dump(fits.string(), FilmView(c.device, c.film), c.provenance); });
    CHECK(fits_card(fits, "DENOISED") == "F");
  }
  CHECK(dev->ledger().category_total(LedgerCategory::DENOISER) == 0);
}

TEST_CASE("session_denoise/failure leaves noisy and one entry") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  cornell::EnvGuard hook("ASTRORAY_TEST_DENOISER_FAIL", "library");
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
  REQUIRE(has(s.active_test_hooks(), "DENOISER_FAIL=library"));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  // d16: not a session failure; the noisy image stays with denoised = false.
  CHECK(s.progress().state == AR_STATE_CONVERGED);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  const std::string error = s.denoiser_error();
  std::printf("session_denoise cpu forced failure: %s\n", error.c_str());
  CHECK(error.find("no library folder given") != std::string::npos);
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE);
  REQUIRE(e.size() == 1);
  CHECK(e[0].detail == error.substr(0, 127));
  CHECK(s.stats().denoises == 0);
}

TEST_CASE("session_denoise/cancel during denoise") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  o.test_hold_denoiser = true;
  Session s(rig.scene, *dev, o);
  s.set_view(cornell::view());
  s.start();
  // Every pass resolved and the session still RENDERING: the render thread is in the final denoise's
  // wait for the state (ruling 87: RENDERING until the denoise ends).
  REQUIRE(slab::until([&] {
    const SessionProgress p = s.progress();
    return p.samples_per_pixel == cornell::kSpp && p.active_generation == rig.generation;
  }));
  CHECK_FALSE(s.wait(100));  // held: the wait does not return
  CHECK(s.progress().state == AR_STATE_RENDERING);
  const auto t0 = std::chrono::steady_clock::now();
  s.cancel();
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(s.wait(2000));
  const auto t2 = std::chrono::steady_clock::now();
  const double cancel_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double paused_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
  std::printf("session_denoise cpu cancel during the denoiser wait: cancel %.3f ms, PAUSED after %.3f ms (B-21 %.0f ms)\n",
              cancel_ms, paused_ms, kB21Ms);
  CHECK(cancel_ms <= kB21Ms);
  CHECK(paused_ms <= kB21Ms);
  CHECK(s.progress().state == AR_STATE_PAUSED);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());  // a cancel is not a failure
}

TEST_CASE("session_denoise/create and destroy while the denoiser loads") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    auto s = std::make_unique<Session>(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
    if (i % 2) s->start();  // half of them with the render thread running too
    s.reset();
    REQUIRE(dev->ledger().category_total(LedgerCategory::DENOISER) == 0);
  }
  std::printf("session_denoise cpu: 20 sessions created and destroyed while loading in %.0f ms\n",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  CHECK(dev->ledger().total() == 0);
}

TEST_CASE("session_denoise/refusals and unknown denoising passes") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp, {"Combined", "Noisy Image", "Denoising Albedo"});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions viewport = cornell::options(AR_DENOISER_OIDN);
  viewport.mode = AR_SESSION_MODE_VIEWPORT;
  CHECK_THROWS_WITH_AS(Session(rig.scene, *dev, viewport), doctest::Contains("viewport OIDN is M2"), SessionUnsupported);
  CHECK_THROWS_AS(Session(rig.scene, *dev, cornell::options(AR_DENOISER_DLSS_RR)), SessionUnsupported);
  // Without a denoiser the denoising passes do not exist: one PASS_UNSUPPORTED entry per name.
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_NONE));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_PASS_UNSUPPORTED);
  CHECK(e.size() == 2);
  for (const char* p : {"Noisy Image", "Denoising Albedo"})
    CHECK(std::count_if(e.begin(), e.end(), [&](const cornell::Entry& x) { return x.detail == p; }) == 1);
}

}  // TEST_SUITE
